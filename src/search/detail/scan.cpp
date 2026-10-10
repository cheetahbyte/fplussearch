#include "search/detail/scan.hpp"

namespace fplussearch::detail {

bool prepare_scope(const Index& ix, Scan& sc) {
  if (sc.scope == UINT32_MAX || sc.scope == 0 || sc.scope_missing) return false;
  const size_t cap = std::min<size_t>(16384, ix.n / 16);
  const uint32_t first = ix.dir_pre[sc.scope], end = ix.dir_end[sc.scope];
  if (end - first > cap) return false;
  for (uint32_t i = first; i < end; ++i) {
    const auto kids = ix.kids(ix.dir_order[i]);
    if (sc.scoped_entries.size() + kids.n > cap) {
      sc.scoped_entries.clear();
      return false;
    }
    for (uint32_t e : kids) sc.scoped_entries.push_back(e);
  }
  if (sc.scoped_entries.empty()) return false;
  if (sc.need_dirs) {
    char buf[288];
    for (uint32_t e : sc.scoped_entries) {
      for (uint32_t a = ix.parent(e);; a = ix.parent(a)) {
        auto [it, inserted] = sc.scoped_hits.try_emplace(a);
        if (!inserted) break;
        NameHit h;
        const std::string_view name = ix.name_view(a, buf);
        sc.name_hit(name, name_mask(name), h);
        it->second.bits = h.bits;
        it->second.neg = h.flags & kNameNeg;
        it->second.best = h.best;
        if (a == 0) break;
      }
    }
  }
  return true;
}

std::vector<Job> make_jobs(const Index& ix, const Scan& sc, size_t target) {
  std::vector<Job> jobs;
  if (!sc.scoped_entries.empty()) {
    const uint32_t count = uint32_t(sc.scoped_entries.size());
    const size_t parts = std::min<size_t>(target, std::max<size_t>(1, count / 256));
    for (size_t j = 0; j < parts; ++j)
      jobs.push_back({-1, true, uint32_t(count * j / parts), uint32_t(count * (j + 1) / parts)});
    return jobs;
  }
  if (!sc.names_matter && sc.neg.empty()) {
    // Filters only: walk entries instead of names.
    for (int s = 0; s < 4; ++s) {
      if (s < 2 ? !sc.dirs_ok : !sc.files_ok) continue;
      const Section& sec = ix.sections[size_t(s)];
      const uint32_t first = sec.first_entry(), count = sec.end_entry() - first;
      const size_t parts = std::max<size_t>(1, target * count / std::max<uint32_t>(1, ix.n));
      for (size_t j = 0; j < parts; ++j) {
        const uint32_t a = first + uint32_t(count * j / parts), b = first + uint32_t(count * (j + 1) / parts);
        if (a < b) jobs.push_back({s, true, a, b});
      }
    }
    return jobs;
  }
  size_t bytes = 0;
  for (int s = 0; s < 4; ++s)
    if (s < 2 ? sc.dirs_ok : sc.files_ok) bytes += ix.sections[size_t(s)].block_off[ix.sections[size_t(s)].blocks()];
  const size_t per_job = std::max<size_t>(1, bytes / target);
  for (int s = 0; s < 4; ++s)
    if (s < 2 ? sc.dirs_ok : sc.files_ok)
      split_blocks(ix.sections[size_t(s)], per_job, [&](uint32_t b, uint32_t e) { jobs.push_back({s, false, b, e}); });
  return jobs;
}

// Positive tokens need scores; negation-only queries need just the union
// of matching directories' subtrees in preorder.
void fill_dir_hits(const Index& ix, Scan& sc, Pool& pool, BigVec<DirHit>& hits, BigVec<uint64_t>& neg_dirs) {
  std::vector<std::pair<int, std::pair<uint32_t, uint32_t>>> jobs;
  const size_t per = std::max<size_t>(1, (ix.sections[0].block_off[ix.sections[0].blocks()] +
                                          ix.sections[1].block_off[ix.sections[1].blocks()]) /
                                             (size_t(pool.size()) * 8));
  for (int s = 0; s < 2; ++s)
    split_blocks(ix.sections[size_t(s)], per, [&](uint32_t b, uint32_t e) { jobs.push_back({s, {b, e}}); });
  if (sc.pos.empty()) {
    neg_dirs.assign((size_t(ix.dirs) + 63) / 64, 0);
    pool.run(jobs.size(), [&](size_t j) {
      sc.candidates(jobs[j].first, jobs[j].second.first, jobs[j].second.second,
                    [&](uint32_t e0, uint32_t c, std::string_view name, uint64_t mask) {
                      NameHit h;
                      if (!sc.name_hit(name, mask, h) || !(h.flags & kNameNeg)) return;
                      for (uint32_t e = e0; e < e0 + c; ++e) {
                        const uint32_t first = ix.dir_pre[e], end = ix.dir_end[e];
                        const uint32_t last = (end - 1) >> 6;
                        for (uint32_t word = first >> 6; word <= last; ++word) {
                          const unsigned lo = word == (first >> 6) ? first & 63 : 0;
                          const unsigned hi = word == last ? ((end - 1) & 63) + 1 : 64;
                          const uint64_t bits = (~uint64_t(0) << lo) & (~uint64_t(0) >> (64 - hi));
                          __atomic_fetch_or(&neg_dirs[word], bits, __ATOMIC_RELAXED);
                        }
                      }
                    }, true);
    });
    sc.neg_dirs = &neg_dirs;
    return;
  }
  hits.assign(ix.dirs, DirHit{});
  pool.run(jobs.size(), [&](size_t j) {
    sc.names(jobs[j].first, jobs[j].second.first, jobs[j].second.second,
             [&](uint32_t e0, uint32_t c, const NameHit& h) {
               DirHit d;
               d.bits = h.bits;
               d.neg = h.flags & kNameNeg;
               d.best = h.best;
               for (uint32_t e = e0; e < e0 + c; ++e) hits[e] = d;
             });
  });
}

}  // namespace fplussearch::detail
