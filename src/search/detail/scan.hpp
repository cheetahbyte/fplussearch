#pragma once

#include <ctime>
#include <unordered_map>

#include "search/detail/bit_filter.hpp"
#include "search/detail/ranking.hpp"
#include "search/engine.hpp"

namespace fplussearch::detail {

// Jobs of roughly equal byte size over the blocks of a section.
template <typename Push>
void split_blocks(const Section& sec, size_t per_job, Push&& push) {
  for (uint32_t b = 0, nb = sec.blocks(); b < nb;) {
    uint32_t e = b + 1;
    while (e < nb && sec.block_off[e] - sec.block_off[b] < per_job) ++e;
    push(b, e);
    b = e;
  }
}

struct Job {
  int section;   // 0-3
  bool entries;  // [a, b) is an entry range (no name predicates), else blocks
  uint32_t a, b;
};

// Everything one query needs while visiting names and entries.
struct Scan {
  const Index& ix;
  const Query& q;
  std::vector<const Token*> pos, neg;
  bool need_dirs = false;
  bool names_matter = false;  // any name predicate: tokens, exts, re:
  bool dirs_ok = true, files_ok = true;
  uint32_t scope = UINT32_MAX;  // in: directory
  bool scope_missing = false;
  uint32_t now = 0;
  std::array<uint8_t, 256> file_ok{};  // per kind byte: 0 reject, 1 accept, 2 check exact size
  bool no_entry_filters = false;       // every entry of a matching name matches
  BigVec<DirHit>* dir_hits = nullptr;
  const BigVec<uint64_t>* neg_dirs = nullptr;
  std::vector<uint32_t> scoped_entries;
  std::unordered_map<uint32_t, DirHit> scoped_hits;
  decltype(Overlay::dead) dead = nullptr;

  Scan(const Index& ix_, const Query& q_) : ix(ix_), q(q_) {
    for (const Token& t : q.tokens) (t.negate ? neg : pos).push_back(&t);
    need_dirs = pos.size() > 1 || !neg.empty();
    names_matter = !q.tokens.empty() || !q.exts.empty() || q.name_re;
    now = uint32_t(std::time(nullptr));
    dirs_ok = (q.types & kTypeDir) && !q.has_size && q.kind != Kind::File && q.kind != Kind::Link;
    files_ok = (q.types & kTypeFiles) && q.kind != Kind::Dir;
    for (int k = 0; k < 256; ++k) {
      const int c = k & 15, cls = k >> 4;
      const uint64_t lo = class_lo(cls), hi = class_hi(cls);
      const uint8_t cls_ok = !q.has_size ? 1 : (hi < q.min_size || lo > q.max_size) ? 0
                             : (q.min_size <= lo && hi <= q.max_size)               ? 1
                                                                                    : 2;
      file_ok[size_t(k)] = ((q.types >> c) & 1) ? cls_ok : 0;
    }
    if (!q.scope.empty()) {
      scope = ix.lookup(q.scope, true);
      scope_missing = scope == UINT32_MAX || !ix.is_dir(scope);
    }
    no_entry_filters = dirs_ok && files_ok && q.kind == Kind::Any && q.types == kTypeAny && !q.has_size &&
                       q.min_mtime == 0 && q.max_mtime == UINT32_MAX && q.scope.empty() && !q.path_re;
  }

  // Name predicates for one distinct name; false if it can't matter.
  bool name_hit(std::string_view name, uint64_t m, NameHit& h) const {
    bool any = pos.empty();
    for (const Token* t : pos) any = any || t->fits(m);
    if (!any) {
      bool n = false;
      for (const Token* t : neg) n = n || t->fits(m);
      if (!n) return false;
    }
    if (!q.exts.empty() && !ext_ok(name, q.exts)) return false;
    if (q.name_re && !q.name_re->matches(name)) return false;
    h = NameHit{};
    h.flags = name_flags(name);
    int32_t total = 0;
    for (size_t t = 0; t < pos.size(); ++t) {
      if (!pos[t]->fits(m)) continue;
      const int32_t s = score_token(name, m, *pos[t]);
      if (s == kNoMatch) continue;
      h.bits |= uint8_t(1u << t);
      total += s;
      if (t < 4) h.best[t] = int16_t(std::clamp(s, 0, 32767));
    }
    h.score = int16_t(std::clamp(total, -32768, 32767));
    for (const Token* t : neg)
      if (t->fits(m) && token_matches(name, *t)) h.flags |= kNameNeg;
    const bool ok = (pos.empty() || h.bits) && !(h.flags & kNameNeg);
    if (ok) h.flags |= kNameOk;
    return ok || h.bits || (h.flags & kNameNeg);
  }

  // Filters that don't depend on the name; true if entry `e` passes.
  bool entry_ok(uint32_t e) const {
    if (e == 0) return false;
    if (dead && ((*dead)[e >> 6] >> (e & 63)) & 1) return false;
    const bool dir = ix.is_dir(e);
    if (dir ? !dirs_ok : !files_ok) return false;
    const uint8_t fl = ix.flags[e];
    if (q.kind == Kind::Link && !(fl & kFlagLink)) return false;
    if (q.kind == Kind::File && (fl & kFlagLink)) return false;
    if (!dir) {
      const uint8_t v = file_ok[ix.file_kind[e - ix.dirs]];
      if (v == 0 || (v == 2 && (ix.size(e) < q.min_size || ix.size(e) > q.max_size))) return false;
    }
    const uint32_t mt = ix.mtime[e];
    if (mt < q.min_mtime || mt > q.max_mtime) return false;
    if (scope != UINT32_MAX && !ix.under(e, scope)) return false;
    return true;
  }

  // Score of entry `e` named by hit `h`, or kNoMatch. Tokens the name lacks
  // must be matched by a folder above it.
  int32_t entry_score(uint32_t e, const NameHit& h) const {
    int32_t score = h.score;
    const uint32_t p = ix.parent(e);
    if (need_dirs && neg_dirs) {
      const uint32_t pre = ix.dir_pre[p];
      if (((*neg_dirs)[pre >> 6] >> (pre & 63)) & 1) return kNoMatch;
    } else if (need_dirs) {
      uint8_t bits = 0;
      std::array<int16_t, 4> best{};
      for (uint32_t a = p;; a = ix.parent(a)) {
        const DirHit& d = scoped_entries.empty() ? (*dir_hits)[a] : scoped_hits.at(a);
        if (d.neg) return kNoMatch;
        bits |= d.bits;
        for (size_t t = 0; t < 4; ++t) best[t] = std::max(best[t], d.best[t]);
        if (a == 0) break;
      }
      const uint8_t all = uint8_t((1u << pos.size()) - 1);
      if (uint8_t(h.bits | bits) != all) return kNoMatch;
      for (size_t t = 0; t < pos.size(); ++t)
        if (!(h.bits & (1u << t))) score += t < 4 ? best[t] * 3 / 4 : 6;
    }
    const bool dir_or_link = ix.is_dir(e) || (ix.flags[e] & kFlagLink);
    return score + ix.dir_prior[p] + rank_tweaks(h.flags, dir_or_link, ix.flags[e], ix.mtime[e], now);
  }

  bool path_ok(uint32_t e) const { return !q.path_re || q.path_re->matches(ix.path(e)); }

  // Visits names in blocks [a, b) of section `s`: for each name that can
  // matter, calls visit(first entry, count, hit).
  // Visits names in blocks [a, b) of section `s` whose mask a token fits:
  // visit(first entry, count, name, mask). Masks are tested 64 names at a
  // time using bit-plane intersections.
  template <typename Visit>
  void candidates(int si, uint32_t a, uint32_t b, Visit&& visit, bool token_only = false) const {
    const Section& s = ix.sections[size_t(si)];
    char buf[256 + 32];
    std::array<const Token*, 16> toks{};
    size_t nt = 0;
    for (const Token* t : pos) toks[nt++] = t;
    for (const Token* t : neg)
      if (nt < toks.size()) toks[nt++] = t;
    const bool all = pos.empty() && (!token_only || nt != neg.size());
    uint64_t next_hits = 0;
    for (uint32_t blk = a; blk < b; ++blk) {
      const uint32_t u0 = blk * kBlock, un = std::min(kBlock, s.names() - u0);
      const uint64_t* masks = s.mask.p + u0;
      uint64_t hits = all ? (un == 64 ? ~uint64_t(0) : (uint64_t(1) << un) - 1) : 0;
      if (!all) {
        // Adjacent blocks share token-mask traversal and expose independent
        // intersections to the compiler.
        if ((blk - a) % 2 == 0) {
          if (blk + 1 < b) {
            const auto pair = fit_blocks<2>({(*s.mask_planes)[blk].data(), (*s.mask_planes)[blk + 1].data()}, toks.data(), nt);
            hits = pair[0];
            next_hits = pair[1];
          } else {
            hits = fit_bits((*s.mask_planes)[blk].data(), un, toks.data(), nt);
          }
        } else {
          hits = next_hits;
        }
        if (un < 64) hits &= (uint64_t(1) << un) - 1;
      }
      if (!hits) continue;
      const char* bs = s.bytes.p + s.block_off[blk];
      const char* be = s.bytes.p + s.block_off[blk + 1];
      uint32_t i = 0;
      uint32_t e = s.name_first ? 0 : s.block_first[blk];
      while (hits) {
        const uint32_t next = uint32_t(__builtin_ctzll(hits));
        if (s.name_first) {
          i = next;
          e = (*s.name_first)[u0 + i];
        } else {
          for (; i < next; ++i) e += s.count(u0 + i);
        }
        const uint32_t k = u0 + i;
        const uint32_t c = s.count(k);
        std::string_view nm;
        if (s.hex) {
          size_t len;
          unpack_hex(bs + s.name_off[k], buf, len);
          nm = {buf, len};
        } else {
          const char* p = bs + s.name_off[k];
          const char* end = (i + 1 < un ? bs + s.name_off[k + 1] : be) - 1;
          nm = {p, size_t(end - p)};
        }
        visit(e, c, nm, masks[next]);
        if (!s.name_first) e += c;
        ++i;
        hits &= hits - 1;
      }
    }
  }

  // Like candidates(), but with the name predicates applied: visit(first
  // entry, count, hit) for each name that can matter.
  template <typename Visit>
  void names(int si, uint32_t a, uint32_t b, Visit&& visit) const {
    candidates(si, a, b, [&](uint32_t e, uint32_t c, std::string_view nm, uint64_t m) {
      NameHit h;
      if (name_hit(nm, m, h)) visit(e, c, h);
    });
  }

  // The most a name could score: lets a search skip scoring names that
  // cannot reach its current top results (it still counts their matches).
  int32_t name_bound(std::string_view name) const {
    int32_t total = 0;
    for (const Token* t : pos) total += token_bound(*t, name);
    return total;
  }

  // Whether a name matches every name predicate (single-token queries).
  bool name_matches(std::string_view name, uint64_t m) const {
    const Token& t = *pos[0];
    if (!t.fits(m)) return false;
    bool hit;
    if (t.mode == Mode::Fuzzy) {
      hit = (t.mask & ~m) == 0 && find_ci(name, t.text) != std::string_view::npos;
      if (!hit && takes_typos(t) && (m & t.start))
        hit = typo_score(name, m, t.text) != kNoMatch || whole_typo_score(name, t.text) != kNoMatch;
    } else {
      hit = score_token(name, m, t) != kNoMatch;
    }
    return hit && (q.exts.empty() || ext_ok(name, q.exts));
  }
};

template <typename Visit>
void scoped_visit(const Index& ix, const Scan& sc, const Job& job, Visit&& visit) {
  char buf[288];
  for (uint32_t i = job.a; i < job.b; ++i) {
    const uint32_t e = sc.scoped_entries[i];
    if (!sc.entry_ok(e)) continue;
    NameHit h;
    if (!sc.names_matter && sc.neg.empty()) {
      h.flags = kNameOk;
      visit(e, h);
      continue;
    }
    const std::string_view name = ix.name_view(e, buf);
    if (sc.name_hit(name, name_mask(name), h) && (h.flags & kNameOk)) visit(e, h);
  }
}

bool prepare_scope(const Index& ix, Scan& sc);
std::vector<Job> make_jobs(const Index& ix, const Scan& sc, size_t target);
void fill_dir_hits(const Index& ix, Scan& sc, Pool& pool, BigVec<DirHit>& hits, BigVec<uint64_t>& neg_dirs);

}  // namespace fplussearch::detail
