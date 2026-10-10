#include "search/matching.hpp"

#include "search/detail/scoring.hpp"

namespace fplussearch {

bool path_matches(const Query& q, std::string_view path) {
  const size_t cut = path.rfind('/');
  const std::string_view name = cut == std::string_view::npos ? path : path.substr(cut + 1);
  const std::string_view dirs = cut == std::string_view::npos ? std::string_view() : path.substr(0, cut);
  if (!q.exts.empty() && !detail::ext_ok(name, q.exts)) return false;
  if ((q.types & kTypeFiles) != kTypeFiles && !((q.types >> categorize(name)) & 1)) return false;
  if (q.name_re && !q.name_re->matches(name)) return false;
  if (q.path_re && !q.path_re->matches(path)) return false;
  const uint64_t m = name_mask(name);
  auto any_comp = [&](auto&& fn) {
    for (size_t i = 0; i < dirs.size();) {
      size_t j = dirs.find('/', i);
      if (j == std::string_view::npos) j = dirs.size();
      if (j > i && fn(dirs.substr(i, j - i))) return true;
      i = j + 1;
    }
    return false;
  };
  for (const Token& t : q.tokens) {
    if (t.negate) {
      if (detail::token_matches(name, t) || any_comp([&](std::string_view c) { return detail::token_matches(c, t); }))
        return false;
    } else if (!(t.fits(m) && token_score(name, m, t) != kNoMatch) &&
               !any_comp([&](std::string_view c) { return token_score(c, ~uint64_t(0), t) != kNoMatch; })) {
      return false;
    }
  }
  return true;
}

int32_t fuzzy_score(std::string_view name, std::string_view q) { return detail::fuzzy_capped(name, q, 100); }

int32_t token_score(std::string_view name, uint64_t m, const Token& t) { return detail::score_token(name, m, t); }

}  // namespace fplussearch
