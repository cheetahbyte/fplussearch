#include "content/query_plan.hpp"

#include <algorithm>

#include "content/grams.hpp"

namespace fplussearch::content_detail {

TQ tq_and(TQ a, TQ b) {
  if (a.kind == TQ::All) return b;
  if (b.kind == TQ::All) return a;
  TQ r;
  r.kind = TQ::And;
  for (TQ* x : {&a, &b}) {
    if (x->kind == TQ::And) r.kids.insert(r.kids.end(), x->kids.begin(), x->kids.end());
    else r.kids.push_back(std::move(*x));
  }
  return r;
}

TQ literal_plan(std::string_view s, bool unicode_caseless, bool blooms) {
  TQ q;
  if (s.size() < 3) return q;
  q.kind = TQ::And;
  for (auto [t, need] : trigrams_small(s, unicode_caseless)) q.kids.push_back({TQ::Tri, t, need, {}});
  if (blooms) {
    for (size_t n : {size_t(5), size_t(7)}) {
      for (size_t i = 0; i + n <= s.size(); ++i) {
        const auto gram = s.substr(i, n);
        if (unicode_caseless && std::any_of(gram.begin(), gram.end(), [](char c) { return unicode_case_byte(uint8_t(c)); })) continue;
        q.kids.push_back({n == 5 ? TQ::Gram5 : TQ::Gram7, gram_hash(gram.data(), n), 0, {}});
      }
    }
  }
  return q.kids.empty() ? TQ{} : q;
}

TQ regex_literal_plan(std::string_view s, bool caseless) {
  if (!caseless) return literal_plan(s, false, true);
  // Enumerate the UTF equivalents of ASCII K/S instead of losing their
  // useful byte constraints. Bound expansion for long runs of these letters.
  std::vector<std::string> literals{std::string()};
  for (char c : s) {
    const bool extra = unicode_case_byte(uint8_t(c));
    if (extra && literals.size() >= 16) return literal_plan(s, true, true);
    const size_t n = literals.size();
    for (size_t i = 0; i < n; ++i) {
      if (extra) literals.push_back(literals[i] + (fold(uint8_t(c)) == 'k' ? "\xE2\x84\xAA" : "\xC5\xBF"));
      literals[i] += c;
    }
  }
  if (literals.size() == 1) return literal_plan(literals[0], false, true);
  TQ q;
  q.kind = TQ::Or;
  for (const auto& literal : literals) q.kids.push_back(literal_plan(literal, false, true));
  return q;
}

TQ bloom_only_plan(const TQ& q) {
  if (q.kind == TQ::Gram5 || q.kind == TQ::Gram7) return q;
  if (q.kind != TQ::And && q.kind != TQ::Or) return {};
  TQ out;
  out.kind = q.kind;
  for (const TQ& child : q.kids) {
    TQ b = bloom_only_plan(child);
    if (b.kind == TQ::All) {
      if (q.kind == TQ::Or) return {};
    } else {
      out.kids.push_back(std::move(b));
    }
  }
  if (out.kids.empty()) return {};
  if (out.kids.size() == 1) return std::move(out.kids[0]);
  return out;
}

}  // namespace fplussearch::content_detail
