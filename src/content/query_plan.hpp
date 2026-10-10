#pragma once

#include <algorithm>
#include <cstdint>
#include <string_view>
#include <vector>

#include "content/bloom.hpp"

namespace fplussearch::content_detail {

// Which documents could possibly match: a tree of trigram tests.
struct TQ {
  enum Kind : uint8_t { All, Tri, And, Or, Gram5, Gram7 } kind = All;
  uint32_t tri = 0;
  uint8_t need = 0;  // follow bits a doc must have for `tri`
  std::vector<TQ> kids;
};

TQ tq_and(TQ a, TQ b);
TQ literal_plan(std::string_view s, bool unicode_caseless = false, bool blooms = false);
TQ regex_literal_plan(std::string_view s, bool caseless);
TQ bloom_only_plan(const TQ& q);
// Preserve alternatives: an unconstrained branch passes, and optional
// fragments never impose a requirement on every match.
inline bool bloom_plan_matches(const BloomView& b, const TQ& q) {
  switch (q.kind) {
    case TQ::Gram5: return bloom_has(b.short_bits, q.tri);
    case TQ::Gram7: return bloom_has(b.long_bits, q.tri);
    case TQ::And: return std::all_of(q.kids.begin(), q.kids.end(), [&](const TQ& child) { return bloom_plan_matches(b, child); });
    case TQ::Or: return std::any_of(q.kids.begin(), q.kids.end(), [&](const TQ& child) { return bloom_plan_matches(b, child); });
    case TQ::All: case TQ::Tri: return true;
  }
  return true;
}

}  // namespace fplussearch::content_detail
