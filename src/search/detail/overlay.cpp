#include "search/detail/overlay.hpp"

#include "search/detail/scan.hpp"

namespace fplussearch::detail {

// Score of an overlay entry, or kNoMatch: the same filters and scoring as
// an index entry, with its path's folders standing in for the folder hits
// and the nearest indexed folder's prior.
int32_t extra_score(const Scan& sc, const Extra& x, std::unordered_map<std::string, int8_t>& priors) {
  const Query& q = sc.q;
  const size_t cut = x.path.rfind('/');
  const std::string_view name = std::string_view(x.path).substr(cut + 1);
  const std::string_view dir = std::string_view(x.path).substr(0, cut == 0 ? 1 : cut);
  if (name.empty()) return kNoMatch;
  if (x.dir ? !sc.dirs_ok : !sc.files_ok) return kNoMatch;
  if (q.kind == Kind::Link && !(x.flags & kFlagLink)) return kNoMatch;
  if (q.kind == Kind::File && (x.flags & kFlagLink)) return kNoMatch;
  if (!x.dir) {
    const uint8_t v = sc.file_ok[x.kind];
    if (v == 0 || (v == 2 && (x.size < q.min_size || x.size > q.max_size))) return kNoMatch;
  }
  if (x.mtime < q.min_mtime || x.mtime > q.max_mtime) return kNoMatch;
  if (!q.scope.empty() && !(std::string_view(x.path).starts_with(q.scope) && x.path.size() > q.scope.size() &&
                            (q.scope == "/" || x.path[q.scope.size()] == '/')))
    return kNoMatch;
  NameHit h;
  if (!sc.name_hit(name, x.mask, h) || !(h.flags & kNameOk)) return kNoMatch;
  int32_t score = h.score;
  if (sc.need_dirs) {
    uint8_t bits = 0;
    std::array<int32_t, 8> best{};
    for (size_t i = 0; i < dir.size();) {
      size_t j = dir.find('/', i);
      if (j == std::string_view::npos) j = dir.size();
      const std::string_view comp = dir.substr(i, j - i);
      i = j + 1;
      if (comp.empty()) continue;
      for (const Token* t : sc.neg)
        if (token_matches(comp, *t)) return kNoMatch;
      for (size_t t = 0; t < sc.pos.size(); ++t) {
        const int32_t sv = score_token(comp, ~uint64_t(0), *sc.pos[t]);
        if (sv == kNoMatch) continue;
        bits |= uint8_t(1u << t);
        best[t] = std::max(best[t], std::clamp(sv, 0, 32767));
      }
    }
    const uint8_t all = uint8_t((1u << sc.pos.size()) - 1);
    if (uint8_t(h.bits | bits) != all) return kNoMatch;
    for (size_t t = 0; t < sc.pos.size(); ++t)
      if (!(h.bits & (1u << t))) score += t < 4 ? best[t] * 3 / 4 : 6;
  }
  if (q.path_re && !q.path_re->matches(x.path)) return kNoMatch;
  // The prior of the nearest folder the index has.
  auto it = priors.find(std::string(dir));
  if (it == priors.end()) {
    int8_t prior = 0;
    for (std::string_view up = dir; !up.empty();) {
      const uint32_t d = sc.ix.lookup(up);
      if (d != UINT32_MAX && sc.ix.is_dir(d)) {
        prior = sc.ix.dir_prior[d];
        break;
      }
      const size_t slash = up.rfind('/');
      if (slash == std::string_view::npos || up == "/") break;
      up = slash == 0 ? std::string_view("/") : up.substr(0, slash);
    }
    it = priors.emplace(std::string(dir), prior).first;
  }
  return score + it->second + rank_tweaks(h.flags, x.dir || (x.flags & kFlagLink), x.flags, x.mtime, sc.now);
}

}  // namespace fplussearch::detail
