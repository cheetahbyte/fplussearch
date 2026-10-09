#include "symindex.hpp"

#include <fcntl.h>
#include <pthread.h>
#include <sys/qos.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <functional>
#include <mutex>
#include <thread>
#include <unordered_map>

#include "store.hpp"

namespace fplussearch {

namespace {

constexpr char kMagic[8] = {'F', 'S', 'R', 'C', 'H', 'S', 'Y', '2'};
enum Field : size_t { kNames = 0, kOcc = kSectionFields, kFileFp, kFileEntry, kFields };
enum Meta : size_t { kMetaBuildId, kMetaFiles };

// Opening and reading 2M small files is bound by kernel and disk latency,
// not CPU, so more threads than cores pay off; past 16 the kernel contends.
constexpr unsigned kReaders = 16;
constexpr size_t kFilesPerJob = 256;

bool attach(SymbolIndex& sx, Fields&& f) {
  SymbolIndex r;
  r.build_id = f.meta[kMetaBuildId];
  if (!f.section(kNames, r.names, false) || !f.get(kOcc, r.occ) || !f.get(kFileFp, r.file_fp) ||
      !f.get(kFileEntry, r.file_entry) || r.file_fp.n != r.file_entry.n)
    return false;
  r.bytes = f.bytes;
  r.backing = std::move(f.backing);
  sx = std::move(r);
  return true;
}

bool map_any(SymbolIndex& sx, const std::string& file) {
  Fields f;
  return map_fields(file, kMagic, kFields, f) && attach(sx, std::move(f));
}

uint64_t mix(uint64_t h, uint64_t v) { return hash_bytes(h ^ (v * 0x9e3779b97f4a7c15ull), {}); }

bool read_file(const std::string& path, uint64_t size, std::vector<char>& buf, std::string_view& out) {
  const int fd = open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) return false;
  fcntl(fd, F_NOCACHE, 1);  // don't evict the user's file cache for a one-off read
  buf.resize(std::min<uint64_t>(size, kMaxSourceSize));
  size_t got = 0;
  while (got < buf.size()) {
    const ssize_t n = read(fd, buf.data() + got, buf.size() - got);
    if (n <= 0) break;
    got += size_t(n);
  }
  close(fd);
  out = std::string_view(buf.data(), got);
  return true;
}

struct Job {
  BigVec<uint32_t> ids;      // symbol ids of newly read files, file after file
  std::vector<uint32_t> counts;   // per file: ids added; UINT32_MAX = unreadable, not recorded
};

}  // namespace

std::string_view SymbolIndex::name(uint32_t u) const {
  const uint32_t b = u / kBlock;
  return std::string_view(names.bytes.p + names.block_off[b] + names.name_off[u]);
}

std::string_view SymbolIndex::symbol(uint32_t o) const {
  const Section& s = names;
  const uint32_t b =
      uint32_t(std::upper_bound(s.block_first.p, s.block_first.p + s.block_first.n, o) - s.block_first.p - 1);
  uint32_t u = b * kBlock, acc = s.block_first[b];
  while (acc + s.count(u) <= o) acc += s.count(u++);
  return name(u);
}

std::string symbol_file(const std::string& cache_file) {
  const size_t dot = cache_file.rfind(".bin");
  return (dot == std::string::npos ? cache_file : cache_file.substr(0, dot)) + ".symbols.bin";
}

bool load_symbols(SymbolIndex& sx, const std::string& file, const Index& ix) {
  SymbolIndex r;
  if (!map_any(r, file) || r.build_id != ix.build_id) return false;
  sx = std::move(r);
  return true;
}

void build_symbols(const Index& ix, const std::string& file, ScanProgress* progress, bool background) {
  const uint32_t* code = ix.code_entry.p;
  const size_t code_count = ix.code_entry.n;
  // Fingerprint each source file by path, size and modification time.
  BigVec<uint64_t> dir_hash(ix.dirs, 0);
  const uint64_t root_hash = hash_bytes(0, ix.root) | 1;
  auto dir_fp = [&](uint32_t d) {
    BigVec<uint32_t> chain;
    while (d != 0 && dir_hash[d] == 0) {
      chain.push_back(d);
      d = ix.parent(d);
    }
    uint64_t h = d == 0 ? root_hash : dir_hash[d];
    for (size_t i = chain.size(); i-- > 0;) h = dir_hash[chain[i]] = hash_bytes(h, ix.name(chain[i])) | 1;
    return h;
  };
  BigVec<uint64_t> fp(code_count);
  for (size_t k = 0; k < code_count; ++k) {
    const uint32_t e = code[k];
    fp[k] = mix(mix(hash_bytes(dir_fp(ix.parent(e)), ix.name(e)), ix.size(e)), ix.code_mtime[k]);
  }
  BigVec<uint64_t>().swap(dir_hash);

  // Files unchanged since the previous symbol index keep their symbols: map
  // its entries to this build's entries by fingerprint.
  SymbolIndex old;
  BigVec<uint8_t> reused(code_count, 0);
  BigVec<uint32_t> old_to_new;  // old entry -> new entry, or UINT32_MAX
  if (map_any(old, file) && old.file_fp.n) {
    BigVec<std::pair<uint64_t, uint32_t>> by_fp(code_count);
    for (uint32_t k = 0; k < code_count; ++k) by_fp[k] = {fp[k], k};
    std::sort(by_fp.begin(), by_fp.end());
    old_to_new.assign(size_t(old.file_entry[old.file_entry.n - 1]) + 1, UINT32_MAX);
    for (size_t j = 0; j < old.file_fp.n; ++j) {
      const auto it = std::lower_bound(by_fp.begin(), by_fp.end(), std::make_pair(old.file_fp[j], uint32_t(0)));
      if (it == by_fp.end() || it->first != old.file_fp[j]) continue;
      reused[it->second] = 1;
      old_to_new[old.file_entry[j]] = code[it->second];
    }
  }
  auto each_old = [&](auto&& f) {  // f(old name, new entry) for every carried-over definition
    uint32_t o = 0;
    for (uint32_t u = 0; u < old.names.names(); ++u)
      for (uint32_t c = old.names.count(u); c--; ++o) {
        const uint32_t e = old.occ[o];
        if (e < old_to_new.size() && old_to_new[e] != UINT32_MAX) f(u, old_to_new[e]);
      }
  };

  // Read and extract the other files, interning names into one table.
  Interner names;
  std::mutex names_m;
  const size_t jobs = (code_count + kFilesPerJob - 1) / kFilesPerJob;
  std::vector<Job> out(jobs);
  std::atomic<size_t> next{0};
  auto worker = [&] {
    if (background) pthread_set_qos_class_self_np(QOS_CLASS_UTILITY, 0);
    std::vector<char> buf;
    std::vector<Symbol> syms;
    for (size_t j; (j = next.fetch_add(1)) < jobs;) {
      Job& job = out[j];
      const size_t k0 = j * kFilesPerJob, k1 = std::min(code_count, k0 + kFilesPerJob);
      for (size_t k = k0; k < k1; ++k) {
        if (progress) progress->source_files.fetch_add(1, std::memory_order_relaxed);
        if (reused[k]) {
          job.counts.push_back(0);
          continue;
        }
        const uint32_t e = code[k];
        std::string_view src;
        if (!read_file(ix.path(e), ix.size(e), buf, src)) {
          job.counts.push_back(UINT32_MAX);
          continue;
        }
        syms.clear();
        if (!looks_minified(src)) extract_symbols(src, lang_of(ix.name(e)), syms);
        std::lock_guard lk(names_m);
        for (const Symbol& s : syms) job.ids.push_back(names.intern(s.name, uint32_t(std::hash<std::string_view>{}(s.name))));
        job.counts.push_back(uint32_t(syms.size()));
      }
    }
  };
  {
    std::vector<std::thread> pool;
    for (unsigned t = 0; t < kReaders; ++t) pool.emplace_back(worker);
    for (auto& t : pool) t.join();
  }
  BigVec<uint32_t> from_old(old.names.name_off.n, UINT32_MAX);
  each_old([&](uint32_t u, uint32_t) {
    if (from_old[u] == UINT32_MAX) {
      const std::string_view nm = old.name(u);
      from_old[u] = names.intern(nm, uint32_t(std::hash<std::string_view>{}(nm)));
    }
  });
  names.drop_table();

  // Sort names and group (name, file) pairs by name.
  const size_t count = names.size();
  BigVec<uint32_t> order(count);
  for (uint32_t i = 0; i < count; ++i) order[i] = i;
  std::sort(order.begin(), order.end(), [&](uint32_t a, uint32_t b) { return names.get(a) < names.get(b); });
  BigVec<uint32_t> rank(count);
  for (uint32_t i = 0; i < count; ++i) rank[order[i]] = i;
  BigVec<uint32_t> first(count + 1, 0);
  for (const Job& job : out)
    for (uint32_t id : job.ids) ++first[rank[id] + 1];
  each_old([&](uint32_t u, uint32_t) { ++first[rank[from_old[u]] + 1]; });
  for (size_t i = 0; i < count; ++i) first[i + 1] += first[i];

  BigVec<uint32_t> occ(first[count]);
  BigVec<uint64_t> file_fp;
  BigVec<uint32_t> file_entry;
  {
    BigVec<uint32_t> pos(first.begin(), first.end() - 1);
    each_old([&](uint32_t u, uint32_t e) { occ[pos[rank[from_old[u]]]++] = e; });
    size_t k = 0;
    for (Job& job : out) {
      size_t at = 0;
      for (uint32_t c : job.counts) {
        if (c != UINT32_MAX) {
          for (uint32_t i = 0; i < c; ++i) occ[pos[rank[job.ids[at + i]]]++] = code[k];
          at += c;
          file_fp.push_back(fp[k]);
          file_entry.push_back(code[k]);
        }
        ++k;
      }
      Job().ids.swap(job.ids);
    }
  }
  for (size_t i = 0; i < count; ++i) std::sort(occ.begin() + first[i], occ.begin() + first[i + 1]);
  BigVec<uint32_t>().swap(rank);
  BigVec<uint32_t>().swap(old_to_new);
  BigVec<uint32_t>().swap(from_old);

  std::vector<Blob> fields(kFields);
  auto section = make_section(count, [&](size_t k) { return names.get(order[k]); }, first.data(), false);
  for (size_t f = 0; f < kSectionFields; ++f) fields[kNames + f] = std::move(section[f]);
  fields[kOcc] = blob(std::move(occ));
  fields[kFileFp] = blob(std::move(file_fp));
  fields[kFileEntry] = blob(std::move(file_entry));
  std::array<uint64_t, 8> meta{};
  meta[kMetaBuildId] = ix.build_id;
  meta[kMetaFiles] = fields[kFileFp].size / sizeof(uint64_t);
  old = {};  // unmap before replacing the file
  save_fields(file, kMagic, meta, fields);
}

std::vector<SymbolLocation> locate_symbols(const std::string& path,
                                         const std::vector<std::string_view>& names) {
  if (names.size() == 1) return {locate_symbol(path, names.front())};
  std::vector<SymbolLocation> locations(names.size());
  if (names.empty()) return locations;
  std::vector<char> buf;
  std::string_view src;
  if (!read_file(path, kMaxSourceSize, buf, src)) return locations;
  const size_t slash = path.rfind('/');
  std::vector<Symbol> syms;
  extract_symbols(src, lang_of(slash == std::string::npos ? path : path.substr(slash + 1)), syms);
  std::unordered_map<std::string_view, SymbolLocation> first;
  first.reserve(names.size());
  for (const std::string_view name : names) first.try_emplace(name);
  for (const Symbol& s : syms) {
    const auto it = first.find(s.name);
    if (it != first.end() && !it->second.found) it->second = {true, s.line, s.kind};
  }
  for (size_t i = 0; i < names.size(); ++i)
    if (const auto it = first.find(names[i]); it != first.end()) locations[i] = it->second;
  return locations;
}

SymbolLocation locate_symbol(const std::string& path, std::string_view name) {
  std::vector<char> buf;
  std::string_view src;
  SymbolLocation loc;
  if (!read_file(path, kMaxSourceSize, buf, src)) return loc;
  const size_t slash = path.rfind('/');
  std::vector<Symbol> syms;
  extract_symbols(src, lang_of(slash == std::string::npos ? path : path.substr(slash + 1)), syms);
  for (const Symbol& s : syms)
    if (s.name == name) return {true, s.line, s.kind};
  return loc;
}

}  // namespace fplussearch
