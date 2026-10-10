#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>

#include "search/detail/substring.hpp"
#include "search/matching.hpp"

namespace fplussearch::detail {

enum class Class : uint8_t { Lower, Upper, Digit, Delim, Other };

inline Class char_class(uint8_t b) {
  if (b >= 'a' && b <= 'z') return Class::Lower;
  if (b >= 'A' && b <= 'Z') return Class::Upper;
  if (b >= '0' && b <= '9') return Class::Digit;
  switch (b) {
    case ' ': case '_': case '-': case '.': case '/': case '(': case ')': case '[': case ']': case ',': case '+':
    case '@':
      return Class::Delim;
    default:
      return Class::Other;
  }
}

constexpr int32_t kScoreMatch = 16;
constexpr int32_t kBonusBoundary = 8, kBonusCamel = 7, kBonusConsec = 4;
constexpr size_t kTypoMinLen = 5;  // fuzzy words this long forgive one typo
constexpr int32_t kTypoCost = 60;  // so clean matches of the same quality rank first

inline int32_t bonus(Class prev, Class cur) {
  if (prev == Class::Delim && cur != Class::Delim) return kBonusBoundary;
  if ((prev == Class::Lower && cur == Class::Upper) ||
      ((prev == Class::Lower || prev == Class::Upper) && cur == Class::Digit))
    return kBonusCamel;
  return 0;
}

inline int32_t length_cost(std::string_view name) { return int32_t(std::min<size_t>(name.size(), 80)) / 3; }

// Where the name's stem ends: before its extension, ignoring a leading dot.
inline size_t stem_end(std::string_view name, size_t off) {
  const size_t dot = name.rfind('.');
  return dot != std::string_view::npos && dot > off ? dot : name.size();
}

inline int32_t single_score(std::string_view name, size_t i, int32_t cap) {
  const Class prev = i == 0 ? Class::Delim : char_class(uint8_t(name[i - 1]));
  int32_t score = kScoreMatch + bonus(prev, char_class(uint8_t(name[i]))) * 2;
  const size_t off = name.size() > 1 && name[0] == '.';
  if (i == off) score += std::min(cap, i + 1 == name.size() ? 100 : i + 1 == stem_end(name, off) ? 80 : 30);
  return score - length_cost(name);
}

inline int32_t fuzzy_capped(std::string_view name, std::string_view q, int32_t cap) {
  if (q.empty()) return kNoMatch;
  const size_t start = find_ci(name, q);
  if (start == std::string_view::npos) return kNoMatch;
  const size_t end = start + q.size() - 1;
  if (q.size() == 1) return single_score(name, start, cap);
  int32_t score = 0, first_bonus = 0;
  for (size_t k = 0; k < q.size(); ++k) {
    const size_t at = start + k;
    const bool run = k > 0;
    const Class prev = at == 0 ? Class::Delim : char_class(uint8_t(name[at - 1]));
    int32_t b = bonus(prev, char_class(uint8_t(name[at])));
    if (run) {
      if (b >= kBonusBoundary && b > first_bonus) first_bonus = b;
      b = std::max({b, first_bonus, kBonusConsec});
    } else {
      first_bonus = b;
    }
    score += kScoreMatch + (k == 0 ? b * 2 : b);
  }
  // Whole-name and stem matches are what people mean most of the time. A
  // leading dot doesn't count: "zshrc" means ~/.zshrc.
  const size_t off = name.size() > 1 && name[0] == '.';
  int32_t placed = 0;
  if (start == off) placed = end + 1 == name.size() ? 100 : end + 1 == stem_end(name, off) ? 80 : 30;
  return score + std::min(placed, cap) - length_cost(name);
}

inline bool starts_fold(std::string_view w, std::string_view q) {
  return w.size() >= q.size() && fold_eq(w.data(), q);
}

inline bool is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

// How long a prefix of `w` the query `q` spells with exactly one edit (a
// wrong, extra, missing or swapped letter), or 0. Digits are never edited:
// "hat_18" is another file than "hat_98", not a typo of it.
inline size_t one_edit_prefix(std::string_view w, std::string_view q) {
  size_t i = 0;
  while (i < q.size() && i < w.size() && fold(uint8_t(w[i])) == uint8_t(q[i])) ++i;
  if (i == q.size()) return 0;  // a clean prefix, not a typo
  const bool query_digit = is_digit(uint8_t(q[i]));
  const bool name_digit = i < w.size() && is_digit(uint8_t(w[i]));
  const std::string_view rest = q.substr(i + 1);
  const std::string_view after = i + 1 <= w.size() ? w.substr(i + 1) : std::string_view();
  if (!query_digit && !name_digit && i + 1 < q.size() && i + 1 < w.size() &&
      !is_digit(uint8_t(q[i + 1])) && !is_digit(uint8_t(w[i + 1])) &&
      fold(uint8_t(w[i])) == uint8_t(q[i + 1]) && fold(uint8_t(w[i + 1])) == uint8_t(q[i]) &&
      starts_fold(w.substr(i + 2), q.substr(i + 2)))
    return q.size();  // swapped
  if (!query_digit && !name_digit && i < w.size() && starts_fold(after, rest)) return q.size();
  if (!query_digit && starts_fold(w.substr(i), rest)) return q.size() - 1;
  if (!name_digit && i < w.size() && starts_fold(after, q.substr(i))) return q.size() + 1;
  return 0;
}

inline int32_t typo_at(std::string_view name, size_t s, std::string_view q) {
  if (s >= name.size() || fold(uint8_t(name[s])) != uint8_t(q[0])) return kNoMatch;
  const size_t n = one_edit_prefix(name.substr(s), q);
  if (n == 0 || n > 128) return kNoMatch;
  char fixed[128];
  for (size_t i = 0; i < n; ++i) fixed[i] = char(fold(uint8_t(name[s + i])));
  const int32_t sc = fuzzy_capped(name, std::string_view(fixed, n), 30);
  return sc == kNoMatch ? kNoMatch : sc - kTypoCost;
}

// Best score for `q` read with one typo at the start of the name or of a
// word in it, scored as if typed right, minus kTypoCost and never placed
// above a prefix ("manif" is "manifest" being typed, not a typo of "manic").
inline int32_t typo_score(std::string_view name, uint64_t m, std::string_view q) {
  int32_t best = typo_at(name, name.size() > 1 && name[0] == '.', q);
  if (m & char_bit(' '))
    for (size_t i = 0; i + 1 < name.size(); ++i)
      if (name[i] == ' ') best = std::max(best, typo_at(name, i + 1, q));
  return best;
}

// Whether `name` is the query `q` with exactly one letter wrong, extra,
// missing or swapped, anywhere. Digits are never edited (see one_edit_prefix).
inline bool one_edit_away(std::string_view name, std::string_view q) {
  const size_t n = name.size(), m = q.size();
  if (n + 1 < m || m + 1 < n) return false;
  size_t i = 0;
  while (i < n && i < m && fold(uint8_t(name[i])) == uint8_t(q[i])) ++i;
  if (i == n && i == m) return false;  // identical: not a typo
  auto same = [&](size_t a, size_t b) {  // name[a..] equals q[b..]
    if (n - a != m - b) return false;
    for (; a < n; ++a, ++b)
      if (fold(uint8_t(name[a])) != uint8_t(q[b])) return false;
    return true;
  };
  if (n == m) {
    if (is_digit(uint8_t(q[i])) || is_digit(uint8_t(name[i]))) return false;
    if (same(i + 1, i + 1)) return true;  // wrong letter
    return i + 1 < n && !is_digit(uint8_t(name[i + 1])) && !is_digit(uint8_t(q[i + 1])) &&
           fold(uint8_t(name[i])) == uint8_t(q[i + 1]) && fold(uint8_t(name[i + 1])) == uint8_t(q[i]) &&
           same(i + 2, i + 2);  // swapped
  }
  return n > m ? !is_digit(uint8_t(name[i])) && same(i + 1, i)
               : !is_digit(uint8_t(q[i])) && same(i, i + 1);
}

// A whole-name match with one typo: scored as if typed right, minus the
// typo cost. This catches edits anywhere, where typo_score only fixes the
// start of a word.
inline int32_t whole_typo_score(std::string_view name, std::string_view q) {
  if (name.size() > 128 || !one_edit_away(name, q)) return kNoMatch;
  char fixed[128];
  for (size_t i = 0; i < name.size(); ++i) fixed[i] = char(fold(uint8_t(name[i])));
  const int32_t sc = fuzzy_capped(name, std::string_view(fixed, name.size()), 100);
  return sc == kNoMatch ? kNoMatch : sc - kTypoCost;
}

inline bool takes_typos(const Token& t) { return t.mode == Mode::Fuzzy && t.text.size() >= kTypoMinLen; }

inline int32_t score_token(std::string_view name, uint64_t m, const Token& t) {
  const int32_t len = length_cost(name);
  switch (t.mode) {
    case Mode::Fuzzy:
      if (takes_typos(t)) {
        const int32_t clean = (t.mask & ~m) == 0 ? fuzzy_capped(name, t.text, 100) : kNoMatch;
        const int32_t typo = (m & t.start) ? std::max(typo_score(name, m, t.text), whole_typo_score(name, t.text))
                                           : kNoMatch;
        return std::max(clean, typo);
      }
      return fuzzy_capped(name, t.text, 100);
    case Mode::Exact: {
      const size_t p = find_ci(name, t.text);
      return p == std::string_view::npos ? kNoMatch : 40 + (p == 0 ? 30 : 0) - len;
    }
    case Mode::Prefix:
      return starts_fold(name, t.text) ? 60 - len : kNoMatch;
    case Mode::Suffix:
      return name.size() >= t.text.size() && fold_eq(name.data() + name.size() - t.text.size(), t.text) ? 50 - len
                                                                                                         : kNoMatch;
  }
  return kNoMatch;
}

inline bool token_matches(std::string_view name, const Token& t) {
  if (t.mode == Mode::Fuzzy) return find_ci(name, t.text) != std::string_view::npos;
  return score_token(name, ~uint64_t(0), t) != kNoMatch;
}

inline bool ext_ok(std::string_view name, const std::vector<std::string>& exts) {
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) return false;
  const std::string_view e = name.substr(dot + 1);
  for (const auto& x : exts)
    if (x.size() == e.size() && fold_eq(e.data(), x)) return true;
  return false;
}

}  // namespace fplussearch::detail
