#include <sys/resource.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <iterator>
#include <optional>
#include <queue>

#include "content/content_index.hpp"
#include "content/matcher.hpp"
#include "content/regex_planner.hpp"
#include "content/segment.hpp"
#include "support/pool.hpp"

namespace fplussearch {

using namespace content_detail;

namespace {

std::atomic<uint64_t> g_diag_ns{0}, g_diag_lists{0}, g_diag_bytes{0}, g_diag_removed{0};
[[maybe_unused]] std::atomic<uint64_t> g_eval_ns{0};

// Docs of segment `s` that may satisfy `q`; nullopt means every doc. Lists
// are applied smallest first, and a large varint list is skipped once the
// candidates are few: a superset only costs a read, decoding costs more.
std::optional<std::vector<uint32_t>> eval(const ContentIndex::Segment& s, const TQ& q) {
  switch (q.kind) {
    case TQ::All: case TQ::Gram5: case TQ::Gram7:
      return std::nullopt;
    case TQ::Tri: {
      const size_t k = s.find(q.tri);
      std::vector<uint32_t> out;
      if (k != SIZE_MAX) s.list(k, out, q.need);
      return out;
    }
    case TQ::Or: {
      std::vector<uint32_t> acc;
      for (const TQ& c : q.kids) {
        auto r = eval(s, c);
        if (!r) return std::nullopt;
        acc.insert(acc.end(), r->begin(), r->end());
      }
      std::sort(acc.begin(), acc.end());
      acc.erase(std::unique(acc.begin(), acc.end()), acc.end());
      return acc;
    }
    case TQ::And:
      break;
  }
  std::vector<size_t> slots;
  std::vector<const TQ*> others;
  std::vector<std::pair<size_t, uint8_t>> found;
  for (const TQ& c : q.kids) {
    if (c.kind == TQ::Tri) {
      const size_t k = s.find(c.tri);
      if (k == SIZE_MAX) return std::vector<uint32_t>{};
      found.push_back({k, c.need});
    } else if (c.kind != TQ::All) {
      others.push_back(&c);
    }
  }
  auto need_of = [&](size_t k) {
    for (auto& f : found)
      if (f.first == k) return f.second;
    return uint8_t(0);
  };
  for (auto& f : found) slots.push_back(f.first);
  std::sort(slots.begin(), slots.end(), [&](size_t a, size_t b) {
    // Bitsets are cheap to test but costly to enumerate: start from a list.
    const size_t ca = s.is_bitset(a) ? SIZE_MAX / 2 + s.list_bytes(a) : s.list_bytes(a);
    const size_t cb = s.is_bitset(b) ? SIZE_MAX / 2 + s.list_bytes(b) : s.list_bytes(b);
    return ca < cb;
  });
  std::optional<std::vector<uint32_t>> acc;
  for (size_t k : slots) {
    if (!acc) {
      acc.emplace();
      s.list(k, *acc, need_of(k));
      continue;
    }
    if (acc->empty()) break;
    if (s.is_bitset(k)) {
      std::erase_if(*acc, [&](uint32_t d) { return !s.has(k, d); });
      continue;
    }
    if (acc->size() <= 4 && s.list_bytes(k) > 4096) {
      static const bool full = getenv("FPLUS_FULL_INTERSECT") != nullptr;
      if (!full) continue;
      const size_t before = acc->size();
      const auto d0 = std::chrono::steady_clock::now();
      s.intersect(k, *acc, need_of(k));
      g_diag_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - d0).count());
      g_diag_lists += 1;
      g_diag_bytes += s.list_bytes(k);
      g_diag_removed += before - acc->size();
      continue;
    }
    s.intersect(k, *acc, need_of(k));
  }
  for (const TQ* c : others) {
    if (acc && acc->empty()) break;
    auto r = eval(s, *c);
    if (!r) continue;
    if (!acc) {
      acc = std::move(r);
      continue;
    }
    std::vector<uint32_t> out;
    std::set_intersection(acc->begin(), acc->end(), r->begin(), r->end(), std::back_inserter(out));
    *acc = std::move(out);
  }
  return acc;
}

// Reads `paths` in order on the pool, until `limit` files matched or the
// budget is spent. Threads claim paths in order, so the result is exactly
// the first matching files of the read prefix.
template <typename PathAt>
GrepResult verify(Pool& pool, const Matcher& m, const GrepOptions& o, size_t path_count, PathAt path_at) {
  GrepResult r;
  r.candidates = path_count;
  const auto t0 = std::chrono::steady_clock::now();
  std::atomic<size_t> next{0}, found{0}, read{0};
  std::atomic<bool> out_of_time{false};
  std::mutex mu;
  std::vector<std::pair<size_t, FileMatches>> hits;
  const size_t workers = pool.size();
  pool.run(workers, [&](size_t) {
    BigVec<char> buf;
    setiopolicy_np(IOPOL_TYPE_VFS_MATERIALIZE_DATALESS_FILES, IOPOL_SCOPE_THREAD, IOPOL_MATERIALIZE_DATALESS_FILES_OFF);
    for (;;) {
      if (found.load(std::memory_order_relaxed) >= o.limit) return;
      if (o.budget_ms > 0 &&
          std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(o.budget_ms)) {
        out_of_time = true;
        return;
      }
      const size_t i = next.fetch_add(1, std::memory_order_relaxed);
      if (i >= path_count) return;
      read.fetch_add(1, std::memory_order_relaxed);
      FileMatches fm;
      if (!match_file(m, std::string(path_at(i)), std::max<size_t>(1, o.per_file), buf, fm)) continue;
      found.fetch_add(1, std::memory_order_relaxed);
      std::lock_guard lk(mu);
      hits.push_back({i, std::move(fm)});
    }
  });
  std::sort(hits.begin(), hits.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
  if (hits.size() > o.limit) hits.resize(o.limit);
  for (auto& h : hits) r.files.push_back(std::move(h.second));
  r.read = read.load();
  r.complete = !out_of_time || r.files.size() >= o.limit;
  return r;
}

}  // namespace

GrepResult ContentIndex::grep(const GrepOptions& o) const {
  GrepResult r;
  Matcher m;
  if (!make_matcher(o, m, r.error)) return r;
  const TQ plan = o.regex ? RegexPlanner(o.pattern, m.caseless).plan() : literal_plan(o.pattern);
  const TQ regex_bloom = o.regex ? bloom_only_plan(plan) : TQ{};
  std::vector<std::pair<uint32_t, uint8_t>> bloom_probes;
  if (!o.regex) {
    for (size_t n : {size_t(5), size_t(7)})
      for (size_t i = 0; i + n <= o.pattern.size(); ++i)
        bloom_probes.push_back({gram_hash(o.pattern.data() + i, n), n == 5 ? 0 : 1});
  }
  auto st = state();
  std::string scope = o.scope;
  while (scope.size() > 1 && scope.back() == '/') scope.pop_back();
  const std::string lo = scope.empty() ? std::string() : scope + "/";
  struct Cand {
    uint64_t priority;  // signed rank, then modification time, descending
    uint32_t order;     // lexicographic path position within the segment
  };
  // Sort each segment using integers; its immutable path permutation supplies
  // the exact lexicographic tie-break without repeatedly comparing strings.
  std::vector<std::vector<Cand>> per(st->segs.size());
  std::lock_guard rl(read_m_);
  readers_->run(st->segs.size(), [&](size_t si) {
    const SegView& v = st->segs[si];
    const Segment& s = *v.seg;
    auto take = [&](uint32_t d) {
      if (v.is_dead(d) || s.rank[d] == kNotText) return;
      const std::string_view p = s.path(d);
      if (!lo.empty() && !p.starts_with(lo)) return;
      if (o.keep && !o.keep(p)) return;
      if (!bloom_probes.empty() && !s.may_contain(d, bloom_probes)) return;
      if (regex_bloom.kind != TQ::All && s.blooms && !bloom_plan_matches((*s.blooms)[d], regex_bloom)) return;
      const uint64_t priority = (uint64_t(int(s.rank[d]) + 128) << 32) | s.mtime[d];
      per[si].push_back({priority, s.path_order[d]});
    };
    if (auto ids = eval(s, plan)) {
      for (uint32_t d : *ids) take(d);
    } else if (lo.empty()) {
      for (uint32_t d = 0; d < s.ndocs; ++d) take(d);
    } else {
      // Every doc in scope: the path-sorted permutation finds the range.
      const uint32_t* bp = s.by_path.p;
      const uint32_t* it = std::lower_bound(bp, bp + s.ndocs, lo, [&](uint32_t d, const std::string& x) { return s.path(d) < x; });
      for (; it != bp + s.ndocs && s.path(*it).starts_with(lo); ++it) take(*it);
    }
    std::sort(per[si].begin(), per[si].end(), [](const Cand& a, const Cand& b) {
      return a.priority != b.priority ? a.priority > b.priority : a.order < b.order;
    });
  });
  size_t candidate_count = 0;
  for (const auto& p : per) candidate_count += p.size();
  std::vector<std::string_view> cands;
  cands.reserve(candidate_count);
  struct Cursor { size_t segment, index; };
  auto path = [&](Cursor c) {
    const Segment& s = *st->segs[c.segment].seg;
    return s.path(s.by_path[per[c.segment][c.index].order]);
  };
  auto worse = [&](Cursor a, Cursor b) {
    const uint64_t pa = per[a.segment][a.index].priority, pb = per[b.segment][b.index].priority;
    return pa != pb ? pa < pb : path(a) > path(b);
  };
  // String comparisons are needed only when merging different segments.
  std::priority_queue<Cursor, std::vector<Cursor>, decltype(worse)> heap(worse);
  for (size_t si = 0; si < per.size(); ++si)
    if (!per[si].empty()) heap.push({si, 0});
  while (!heap.empty()) {
    Cursor c = heap.top();
    heap.pop();
    cands.push_back(path(c));
    if (++c.index < per[c.segment].size()) heap.push(c);
  }
  for (auto& p : per) std::vector<Cand>().swap(p);
  return verify(*readers_, m, o, cands.size(), [&](size_t i) { return cands[i]; });
}

GrepResult ContentIndex::grep_files(const GrepOptions& o, const std::vector<std::string>& paths) const {
  GrepResult r;
  Matcher m;
  if (!make_matcher(o, m, r.error)) return r;
  std::vector<std::string_view> v;
  for (const auto& p : paths)
    if (!o.keep || o.keep(p)) v.push_back(p);
  std::lock_guard rl(read_m_);
  return verify(*readers_, m, o, v.size(), [&](size_t i) { return v[i]; });
}

}  // namespace fplussearch
