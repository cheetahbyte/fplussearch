#include "search/engine.hpp"

#include "search/detail/scan.hpp"
#include "symbols/symbol_index.hpp"

namespace fplussearch {

using namespace detail;

Results Engine::search_symbols(const Index& ix, const SymbolIndex& sx, const Query& q, size_t limit) {
  Results r;
  if (q.syms.empty()) return r;
  Query fq = q;
  fq.syms.clear();
  const bool by_file = fq.selects();

  // Symbol names must contain every sym: term; the rest of the query picks
  // the files that define them.
  std::string_view needle;
  int skip = -1;
  for (int i = 0; i < int(q.syms.size()); ++i)
    if (q.syms[i].size() > needle.size()) needle = q.syms[i], skip = i;
  auto sym_ok = [&](std::string_view name) {
    for (int i = 0; i < int(q.syms.size()); ++i)
      if (i != skip && !contains_fold(name, q.syms[i])) return false;
    return true;
  };
  std::vector<uint64_t> files;
  if (by_file) {
    files.assign((ix.count() + 63) / 64, 0);
    each_match(ix, fq, true, [&](uint32_t e) {
      __atomic_fetch_or(&files[e / 64], uint64_t(1) << (e % 64), __ATOMIC_RELAXED);
    });
  }

  const Section& s = sx.names;
  std::vector<std::pair<uint32_t, uint32_t>> jobs;
  const size_t per_job = std::max<size_t>(1, s.block_off[s.blocks()] / (size_t(pool_.size()) * 24));
  split_blocks(s, per_job, [&](uint32_t b, uint32_t e) { jobs.emplace_back(b, e); });
  struct alignas(128) Out {
    uint64_t count = 0;
    std::vector<uint32_t> hits;
  };
  std::vector<Out> out(jobs.size());
  pool_.run(jobs.size(), [&](size_t j) {
    Out& o = out[j];
    auto take = [&](uint32_t o0, uint32_t o1) {
      for (uint32_t i = o0; i < o1; ++i) {
        const uint32_t e = sx.occ[i];
        if (by_file && !((files[e / 64] >> (e % 64)) & 1)) continue;
        ++o.count;
        if (o.hits.size() < limit) o.hits.push_back(i);
      }
    };
    for (uint32_t b = jobs[j].first; b < jobs[j].second; ++b) {
      const uint32_t u0 = b * kBlock, un = std::min(kBlock, s.names() - u0);
      const char* bs = s.bytes.p + s.block_off[b];
      const char* be = s.bytes.p + s.block_off[b + 1];
      const uint16_t* off = s.name_off.p + u0;
      auto name_at = [&](uint32_t i) {
        const char* p = bs + off[i];
        const char* end = (i + 1 < un ? bs + off[i + 1] : be) - 1;
        return std::string_view(p, size_t(end - p));
      };
      uint32_t e = s.block_first[b], i = 0;
      const char* p = bs;
      while ((p = find_fold(p, be, needle)) != be) {
        const uint32_t h = uint32_t(p - bs);
        while (i + 1 < un && off[i + 1] <= h) e += s.count(u0 + i++);
        const uint32_t c = s.count(u0 + i);
        if (sym_ok(name_at(i))) take(e, e + c);
        if (++i >= un) break;
        e += c;
        p = bs + off[i];
      }
    }
  });
  for (auto& o : out) {
    r.total += o.count;
    for (uint32_t i : o.hits) {
      if (r.top.size() >= limit) break;
      r.top.push_back(i);
    }
  }
  return r;
}

}  // namespace fplussearch
