#include <pthread.h>
#include <sys/qos.h>
#include <sys/resource.h>
#include <sys/stat.h>

#include <atomic>
#include <thread>

#include "content/content_index.hpp"
#include "content/document.hpp"
#include "content/eligibility.hpp"
#include "content/file_io.hpp"
#include "content/grams.hpp"
#include "content/segment_io.hpp"

namespace fplussearch {

using namespace content_detail;

namespace {

constexpr uint64_t kBatchBytes = uint64_t(32) << 20;  // file bytes per segment build

void set_background(bool background) {
  pthread_set_qos_class_self_np(background ? QOS_CLASS_UTILITY : QOS_CLASS_USER_INITIATED, 0);
  // Never download a cloud placeholder just to index it.
  setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
}

// Runs fn(i) for i in [0, n) on `threads` threads, claiming small chunks.
template <typename Fn>
void parallel(size_t n, unsigned threads, bool background, size_t chunk, Fn&& fn) {
  std::atomic<size_t> next{0};
  auto work = [&](unsigned t) {
    set_background(background);
    for (size_t a; (a = next.fetch_add(chunk)) < n;)
      for (size_t i = a; i < std::min(n, a + chunk); ++i) fn(t, i);
  };
  std::vector<std::thread> pool;
  for (unsigned t = 1; t < threads; ++t) pool.emplace_back(work, t);
  work(0);
  for (auto& th : pool) th.join();
}

}  // namespace

void ContentIndex::add_docs(std::vector<DocIn>& docs, bool background) {
  if (docs.empty()) return;
  const unsigned threads = std::min(12u, std::max(1u, std::thread::hardware_concurrency()));
  BigVec<uint32_t> count(size_t(1) << 22, 0);  // reused per shard: trigram -> slot, then count
  for (size_t b0 = 0; b0 < docs.size();) {
    size_t b1 = b0;
    for (uint64_t bytes = 0; b1 < docs.size() && (bytes < kBatchBytes || b1 == b0); ++b1) bytes += docs[b1].size;
    const size_t n = b1 - b0;
    // Read and extract in parallel; each thread keeps its own trigram runs.
    struct Local {
      std::vector<uint32_t> scratch;
      BigVec<char> buf;
      BigVec<uint32_t> flat;  // mapped directly, so freeing it returns the memory
    };
    std::vector<Local> locals(threads);
    struct Run {
      uint32_t thread = 0, start = 0, len = 0;
      int8_t rank = kNotText;
      uint64_t size = 0;
      uint32_t mtime = 0;
      Bloom bloom;
      uint64_t mtime_ns = 0, ctime_ns = 0;
    };
    std::vector<Run> runs(n);
    parallel(n, threads, background, 16, [&](unsigned t, size_t i) {
      Local& L = locals[t];
      const DocIn& d = docs[b0 + i];
      Run& r = runs[i];
      r.thread = t;
      r.start = uint32_t(L.flat.size());
      r.size = d.size, r.mtime = d.mtime;
      struct stat st{};
      if (!read_file(std::string(d.path).c_str(), L.buf, kMaxContentFile + 1, &st)) return;
      r.size = uint64_t(st.st_size), r.mtime = uint32_t(st.st_mtimespec.tv_sec);
      r.mtime_ns = time_ns(st.st_mtimespec);
      r.ctime_ns = time_ns(st.st_ctimespec);
      if (L.buf.size() > kMaxContentFile || looks_binary(L.buf)) return;
      trigrams(L.buf.data(), L.buf.size(), L.scratch, L.flat);
      make_bloom(std::string_view(L.buf.data(), L.buf.size()), r.bloom);
      r.len = uint32_t(L.flat.size() - r.start);
      r.rank = doc_rank(d.path);
    });
    for (Local& L : locals) std::vector<uint32_t>().swap(L.scratch), BigVec<char>().swap(L.buf);

    // Counting sort of (trigram, doc) pairs, a quarter of the trigram space
    // at a time: each doc's trigrams are sorted, so a cursor per doc walks
    // through them shard by shard, and only one shard's pairs are held.
    constexpr uint32_t kShardBits = 22, kShards = 1u << (24 - kShardBits);
    auto tris = [&](size_t i) {
      const Run& r = runs[i];
      return std::pair(locals[r.thread].flat.data() + r.start, r.len);
    };
    std::vector<uint32_t> cursor(n, 0), shard_end(n, 0), keys;
    BigVec<uint64_t> start;
    BigVec<uint32_t> raw;
    BigVec<uint8_t> raw_mask;
    auto load_shard = [&](uint32_t sh) {
      std::fill(count.begin(), count.end(), 0);
      for (size_t i = 0; i < n; ++i) {
        auto [p, len] = tris(i);
        uint32_t j = cursor[i];
        for (; j < len && (p[j] >> (8 + kShardBits)) == sh; ++j) ++count[(p[j] >> 8) & ((1u << kShardBits) - 1)];
        shard_end[i] = j;
      }
      keys.clear();
      start.assign(1, 0);
      for (uint32_t t = 0; t < (1u << kShardBits); ++t) {
        if (!count[t]) continue;
        start.push_back(start.back() + count[t]);
        count[t] = uint32_t(keys.size());
        keys.push_back(sh << kShardBits | t);
      }
      raw.assign(start.back(), 0);
      raw_mask.assign(start.back(), 0);
      for (size_t i = 0; i < n; ++i) {
        auto [p, len] = tris(i);
        for (uint32_t j = cursor[i]; j < shard_end[i]; ++j) {
          const uint64_t a = start[count[(p[j] >> 8) & ((1u << kShardBits) - 1)]]++;
          raw[a] = uint32_t(i);
          raw_mask[a] = uint8_t(p[j]);
        }
        cursor[i] = shard_end[i];
      }
      // Insertion cursors now hold bucket ends; shift to recover starts.
      for (size_t k = start.size() - 1; k > 0; --k) start[k] = start[k - 1];
      start[0] = 0;
    };
    std::vector<DocMeta> meta(n);
    std::vector<BloomView> blooms(n);
    for (size_t i = 0; i < n; ++i) meta[i] = {docs[b0 + i].path, runs[i].size, runs[i].mtime, runs[i].rank};
    for (size_t i = 0; i < n; ++i) blooms[i] = {runs[i].bloom.short_bits, runs[i].bloom.long_bits, runs[i].mtime_ns, runs[i].ctime_ns};
    const uint64_t id = next_id();
    size_t k = 0;
    uint32_t shard = 0;
    load_shard(0);
    const bool ok = write_segment(dir_, id, meta, [&](std::span<const uint32_t>& list, std::span<const uint8_t>& masks, uint32_t& t) {
      while (k >= keys.size()) {
        if (++shard >= kShards) return false;
        load_shard(shard);
        k = 0;
      }
      t = keys[k];
      list = std::span<const uint32_t>(raw.data(), raw.size()).subspan(start[k], start[k + 1] - start[k]);
      masks = std::span<const uint8_t>(raw_mask.data(), raw_mask.size()).subspan(start[k], start[k + 1] - start[k]);
      ++k;
      return true;
    }, blooms);
    for (Local& L : locals) BigVec<uint32_t>().swap(L.flat);
    if (ok)
      if (auto seg = map_segment(dir_, id)) {
        auto next = std::make_shared<State>(*state());
        auto dead = std::make_shared<std::vector<uint64_t>>((seg->ndocs + 63) / 64, 0);
        next->segs.push_back({std::move(seg), std::move(dead), n});
        save_manifest(*next);
        publish(std::move(next));
      }
    b0 = b1;
  }
}

}  // namespace fplussearch
