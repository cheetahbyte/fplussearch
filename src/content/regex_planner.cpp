#include "content/regex_planner.hpp"

#include <algorithm>

#include "content/grams.hpp"

namespace fplussearch::content_detail {

namespace {

constexpr size_t kMaxExact = 16;

TQ exact_query(const std::optional<std::vector<std::string>>& set, bool unicode_caseless = false) {
  TQ q;
  if (!set || set->empty()) return q;
  for (const auto& s : *set)
    if (s.size() < 3) return q;
  if (set->size() == 1) return regex_literal_plan((*set)[0], unicode_caseless);
  q.kind = TQ::Or;
  for (const auto& s : *set) q.kids.push_back(regex_literal_plan(s, unicode_caseless));
  return q;
}

Info info_all() { return {std::nullopt, TQ{}}; }
Info info_exact(std::vector<std::string> v) { return {std::move(v), TQ{}}; }

void dedup(std::vector<std::string>& v) {
  std::sort(v.begin(), v.end());
  v.erase(std::unique(v.begin(), v.end()), v.end());
}

Info concat(Info cur, Info n, bool unicode_caseless = false) {
  if (cur.exact && n.exact && cur.exact->size() * n.exact->size() <= kMaxExact) {
    std::vector<std::string> set;
    for (const auto& a : *cur.exact)
      for (const auto& b : *n.exact) set.push_back(a + b);
    dedup(set);
    return {std::move(set), tq_and(std::move(cur.q), std::move(n.q))};
  }
  TQ q = tq_and(tq_and(std::move(cur.q), exact_query(cur.exact, unicode_caseless)), std::move(n.q));
  if (n.exact && n.exact->size() <= kMaxExact) return {std::move(n.exact), std::move(q)};
  return {std::nullopt, tq_and(std::move(q), exact_query(n.exact, unicode_caseless))};
}

Info alternate(std::vector<Info> parts, bool unicode_caseless = false) {
  bool all_exact = true;
  for (const Info& p : parts) all_exact = all_exact && p.exact.has_value();
  if (all_exact) {
    std::vector<std::string> set;
    for (const Info& p : parts) set.insert(set.end(), p.exact->begin(), p.exact->end());
    dedup(set);
    if (set.size() <= kMaxExact) return info_exact(std::move(set));
  }
  TQ ors;
  ors.kind = TQ::Or;
  for (Info& p : parts) {
    TQ q = tq_and(std::move(p.q), exact_query(p.exact, unicode_caseless));
    if (q.kind == TQ::All) return info_all();
    ors.kids.push_back(std::move(q));
  }
  return {std::nullopt, std::move(ors)};
}

}  // namespace

RegexPlanner::RegexPlanner(std::string_view p, bool caseless) : p_(p), caseless_(caseless) {
  safe_ = std::all_of(p.begin(), p.end(), [](char c) { return uint8_t(c) < 0x80; }) && p.find("(*") == std::string_view::npos;
}
TQ RegexPlanner::plan() {
  Info i = alternation();
  if (!safe_ || i_ != p_.size()) return TQ{};  // unsupported or unbalanced: give up safely
  return tq_and(std::move(i.q), exact_query(i.exact, caseless_));
}

Info RegexPlanner::alternation() {
  std::vector<Info> parts{sequence()};
  while (i_ < p_.size() && p_[i_] == '|') {
    ++i_;
    parts.push_back(sequence());
  }
  return parts.size() == 1 ? std::move(parts[0]) : alternate(std::move(parts), caseless_);
}

Info RegexPlanner::sequence() {
  Info cur = info_exact({""});
  while (i_ < p_.size() && p_[i_] != '|' && p_[i_] != ')') cur = concat(std::move(cur), repeat(), caseless_);
  return cur;
}

Info RegexPlanner::repeat() {
  Info a = atom();
  for (;;) {
    if (i_ >= p_.size()) return a;
    const char c = p_[i_];
    int lo = -1;
    if (c == '*' || c == '?') lo = 0, ++i_;
    else if (c == '+') lo = 1, ++i_;
    else if (c == '{') {
      size_t j = i_ + 1, n = 0;
      bool digits = false;
      while (j < p_.size() && p_[j] >= '0' && p_[j] <= '9') n = n * 10 + size_t(p_[j++] - '0'), digits = true;
      const size_t close = p_.find('}', i_);
      if (!digits || close == std::string_view::npos) return a;  // a literal '{'
      lo = int(std::min<size_t>(n, 2));
      i_ = close + 1;
    } else {
      return a;
    }
    if (i_ < p_.size() && (p_[i_] == '?' || p_[i_] == '+')) ++i_;  // lazy / possessive
    if (lo == 0) a = info_all();
    else a = {std::nullopt, tq_and(std::move(a.q), exact_query(a.exact, caseless_))};  // at least one copy appears
  }
}

Info RegexPlanner::atom() {
  const char c = p_[i_++];
  switch (c) {
    case '.':
      return info_all();
    case '^': case '$':
      return info_exact({""});
    case '(': {
      bool look = false;
      if (i_ < p_.size() && p_[i_] == '?') {
        ++i_;
        // Inline modes, assertions and extensions need richer PCRE
        // semantics. Let fresh-file verification handle them directly.
        if (i_ >= p_.size() || p_[i_] != ':') safe_ = false;
        if (i_ < p_.size() && (p_[i_] == '=' || p_[i_] == '!')) look = true, ++i_;
        else if (i_ + 1 < p_.size() && p_[i_] == '<' && (p_[i_ + 1] == '=' || p_[i_ + 1] == '!')) look = true, i_ += 2;
        else if (i_ < p_.size() && (p_[i_] == '<' || p_[i_] == 'P' || p_[i_] == '\'')) {
          const size_t close = p_.find_first_of(">'", i_ + 1);
          i_ = close == std::string_view::npos ? p_.size() : close + 1;
        } else {
          // (?:  (?i)  (?i:  flags: folding already ignores case.
          while (i_ < p_.size() && p_[i_] != ':' && p_[i_] != ')') ++i_;
          if (i_ < p_.size() && p_[i_] == ')') {
            ++i_;
            return info_exact({""});
          }
          if (i_ < p_.size()) ++i_;
        }
      }
      Info in = alternation();
      if (i_ < p_.size() && p_[i_] == ')') ++i_;
      else i_ = p_.size() + 1;  // unbalanced
      return look ? info_exact({""}) : in;
    }
    case '[':
      return char_class();
    case '\\':
      return escape();
    default:
      return info_exact({std::string(1, char(fold(uint8_t(c))))});
  }
}

Info RegexPlanner::escape() {
  if (i_ >= p_.size()) return info_all();
  const char c = p_[i_++];
  switch (c) {
    case 'b': case 'B': case 'A': case 'z': case 'Z': case 'G': case 'K':
      return info_exact({""});
    case 'n': return info_exact({"\n"});
    case 't': return info_exact({"\t"});
    case 'r': return info_exact({"\r"});
    case 'Q': {
      const size_t end = p_.find("\\E", i_);
      std::string lit(p_.substr(i_, end == std::string_view::npos ? std::string_view::npos : end - i_));
      i_ = end == std::string_view::npos ? p_.size() : end + 2;
      for (auto& ch : lit) ch = char(fold(uint8_t(ch)));
      return info_exact({lit});
    }
    case 'x': case 'p': case 'P': case 'N':
      if (i_ < p_.size() && p_[i_] == '{') {
        const size_t close = p_.find('}', i_);
        i_ = close == std::string_view::npos ? p_.size() : close + 1;
      } else if (c == 'x') {
        i_ = std::min(p_.size(), i_ + 2);
      } else if (c != 'N') {
        i_ = std::min(p_.size(), i_ + 1);
      }
      return info_all();
    default:
      if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9')) {
        if (std::string_view("dDsSwWhHvVRXC").find(c) == std::string_view::npos) safe_ = false;
        return info_all();
      }
      return info_exact({std::string(1, char(fold(uint8_t(c))))});  // escaped punctuation
  }
}

// Up to 8 literal members become exact strings; anything else matches any.
Info RegexPlanner::char_class() {
  bool negated = false, plain = true;
  if (i_ < p_.size() && p_[i_] == '^') negated = true, ++i_;
  std::vector<std::string> set;
  bool first = true;
  while (i_ < p_.size() && (p_[i_] != ']' || first)) {
    first = false;
    char c = p_[i_++];
    if (c == '[' && i_ < p_.size() && p_[i_] == ':') {
      const size_t close = p_.find(":]", i_);
      i_ = close == std::string_view::npos ? p_.size() : close + 2;
      plain = false;
      continue;
    }
    if (c == '\\') {
      if (i_ >= p_.size()) break;
      const char e = p_[i_++];
      if ((e >= 'a' && e <= 'z') || (e >= 'A' && e <= 'Z') || (e >= '0' && e <= '9')) {
        plain = false;
        continue;
      }
      c = e;
    }
    if (i_ + 1 < p_.size() && p_[i_] == '-' && p_[i_ + 1] != ']') {
      const char hi = p_[i_ + 1];
      i_ += 2;
      if (uint8_t(hi) < uint8_t(c) || uint8_t(hi) - uint8_t(c) > 8) {
        plain = false;
        continue;
      }
      for (int x = uint8_t(c); x <= uint8_t(hi); ++x) set.push_back(std::string(1, char(fold(uint8_t(x)))));
      continue;
    }
    set.push_back(std::string(1, char(fold(uint8_t(c)))));
  }
  if (i_ < p_.size()) ++i_;  // ']'
  dedup(set);
  if (negated || !plain || set.empty() || set.size() > 8) return info_all();
  return info_exact(std::move(set));
}

}  // namespace fplussearch::content_detail
