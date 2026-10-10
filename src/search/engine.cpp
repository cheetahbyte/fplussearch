#include "search/engine.hpp"

#include <atomic>

#include "search/detail/overlay.hpp"
#include "search/detail/scan.hpp"

namespace fplussearch {

using namespace detail;

void Engine::each_match(const Index& ix, const Query& q, bool files_only, const std::function<void(uint32_t)>& take) {
  if (ix.count() == 0) return;
  Scan sc(ix, q);
  if (sc.scope_missing) return;
  if (files_only) sc.dirs_ok = false;
  BigVec<DirHit> dir_hits;
  BigVec<uint64_t> neg_dirs;
  const bool selective = prepare_scope(ix, sc);
  if (sc.need_dirs && !selective) {
    fill_dir_hits(ix, sc, pool_, dir_hits, neg_dirs);
    sc.dir_hits = &dir_hits;
  }
  const std::vector<Job> jobs = make_jobs(ix, sc, size_t(pool_.size()) * 24);
  NameHit any;
  any.flags = kNameOk;
  pool_.run(jobs.size(), [&](size_t j) {
    const Job& job = jobs[j];
    auto entry = [&](uint32_t e, const NameHit& h) {
      if (sc.entry_ok(e) && sc.entry_score(e, h) != kNoMatch && sc.path_ok(e)) take(e);
    };
    if (job.section == -1) {
      scoped_visit(ix, sc, job, entry);
      return;
    }
    if (job.entries) {
      for (uint32_t e = job.a; e < job.b; ++e) entry(e, any);
      return;
    }
    sc.names(job.section, job.a, job.b, [&](uint32_t e0, uint32_t c, const NameHit& h) {
      if (!(h.flags & kNameOk)) return;
      for (uint32_t e = e0; e < e0 + c; ++e) entry(e, h);
    });
  });
}

Results Engine::search(const Index& ix, const Query& q, size_t limit, const Overlay* overlay) {
  Results r;
  if (ix.count() == 0 || !q.selects()) return r;
  Scan sc(ix, q);
  if (overlay) sc.dead = overlay->dead;
  // New folders aren't in the index yet; their entries are in the overlay.
  const bool scope_new = sc.scope_missing && overlay && !overlay->extras.empty();
  if (sc.scope_missing && !scope_new) return r;
  BigVec<DirHit> dir_hits;
  BigVec<uint64_t> neg_dirs;
  const bool selective = prepare_scope(ix, sc);
  if (sc.need_dirs && !selective) {
    fill_dir_hits(ix, sc, pool_, dir_hits, neg_dirs);
    sc.dir_hits = &dir_hits;
  }
  const std::vector<Job> jobs = scope_new ? std::vector<Job>() : make_jobs(ix, sc, size_t(pool_.size()) * 8);
  std::vector<Chunk> out(jobs.size());
  for (auto& c : out) c.top = TopK(limit);
  // Names that can't beat the k-th best result so far are only counted. The
  // floor is shared: every job's k-th best bounds the overall k-th best.
  const bool prune = limit > 0 && sc.pos.size() == 1 && !sc.need_dirs && !q.name_re && !q.path_re;
  std::atomic<int32_t> shared_floor{INT32_MIN};
  NameHit any;
  any.flags = kNameOk;
  pool_.run(jobs.size(), [&](size_t j) {
    const Job& job = jobs[j];
    Chunk& o = out[j];
    auto entry = [&](uint32_t e, const NameHit& h) {
      if (!sc.entry_ok(e)) return;
      const int32_t s = sc.entry_score(e, h);
      if (s == kNoMatch || !sc.path_ok(e)) return;
      ++o.count;
      o.top.push(rank_key(s, ix.dir_pre[ix.parent(e)], e));
    };
    auto publish_floor = [&] {
      if (!o.top.full()) return;
      const int32_t f = key_score(o.top.floor);
      for (int32_t cur = shared_floor.load(std::memory_order_relaxed);
           f > cur && !shared_floor.compare_exchange_weak(cur, f, std::memory_order_relaxed);) {
      }
    };
    if (job.section == -1) {
      scoped_visit(ix, sc, job, entry);
    } else if (job.entries) {
      for (uint32_t e = job.a; e < job.b; ++e) entry(e, any);
    } else {
      size_t since = 0;
      sc.candidates(job.section, job.a, job.b, [&](uint32_t e0, uint32_t c, std::string_view nm, uint64_t m) {
        if (prune && sc.name_bound(nm) + kMaxEntryBonus < shared_floor.load(std::memory_order_relaxed)) {
          if (!sc.name_matches(nm, m)) return;
          if (sc.no_entry_filters) o.count += c;
          else
            for (uint32_t e = e0; e < e0 + c; ++e) o.count += sc.entry_ok(e);
          return;
        }
        NameHit h;
        if (!sc.name_hit(nm, m, h) || !(h.flags & kNameOk)) return;
        for (uint32_t e = e0; e < e0 + c; ++e) entry(e, h);
        if (prune && (since += c) >= 256) since = 0, publish_floor();
      });
    }
    o.top.cut();
    publish_floor();
  });
  std::vector<Key> keys;
  for (auto& o : out) {
    r.total += o.count;
    keys.insert(keys.end(), o.top.buf.begin(), o.top.buf.end());
  }
  const size_t k = std::min(limit, keys.size());
  std::partial_sort(keys.begin(), keys.begin() + long(k), keys.end(), std::greater<>());
  // Overlay entries are few: score them all and merge by score.
  std::vector<std::pair<int32_t, const Extra*>> extras;
  if (overlay) {
    std::unordered_map<std::string, int8_t> priors;
    for (const Extra* x : overlay->extras) {
      const int32_t s = extra_score(sc, *x, priors);
      if (s == kNoMatch) continue;
      ++r.total;
      extras.push_back({s, x});
    }
    std::stable_sort(extras.begin(), extras.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first > b.first : a.second->path < b.second->path;
    });
  }
  size_t i = 0, j = 0;
  while (r.top.size() < limit && (i < k || j < extras.size())) {
    if (j >= extras.size() || (i < k && key_score(keys[i]) >= extras[j].first)) {
      r.top.push_back(key_entry(keys[i]));
      r.score.push_back(key_score(keys[i]));
      r.extra.push_back(nullptr);
      ++i;
    } else {
      r.top.push_back(UINT32_MAX);
      r.score.push_back(extras[j].first);
      r.extra.push_back(extras[j].second);
      ++j;
    }
  }
  return r;
}

}  // namespace fplussearch
