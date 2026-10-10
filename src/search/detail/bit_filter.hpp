#pragma once

#include <array>
#include <bit>
#include <cstdint>

#include "search/query.hpp"

namespace fplussearch::detail {

template <size_t Width>
std::array<uint64_t, Width> fit_blocks(const std::array<const uint64_t*, Width>& planes, const Token* const* toks, size_t nt) {
  std::array<uint64_t, Width> hits{};
  for (size_t t = 0; t < nt; ++t) {
    const Token& token = *toks[t];
    uint64_t mask = token.mask;
    std::array<uint64_t, Width> required;
    required.fill(~uint64_t(0));
    if (token.loose == 0) {
      while (mask) {
        const unsigned bit = std::countr_zero(mask);
        for (size_t lane = 0; lane < Width; ++lane) required[lane] &= planes[lane][bit];
        mask &= mask - 1;
      }
      for (size_t lane = 0; lane < Width; ++lane) hits[lane] |= required[lane];
      continue;
    }
    mask = token.mask & ~token.loose;
    while (mask) {
      const unsigned bit = std::countr_zero(mask);
      for (size_t lane = 0; lane < Width; ++lane) required[lane] &= planes[lane][bit];
      mask &= mask - 1;
    }
    auto zero = required, one = required;
    mask = token.mask & token.loose;
    while (mask) {
      const unsigned bit = std::countr_zero(mask);
      for (size_t lane = 0; lane < Width; ++lane) {
        const uint64_t present = planes[lane][bit];
        one[lane] = (one[lane] & present) | zero[lane];
        zero[lane] &= present;
      }
      mask &= mask - 1;
    }
    std::array<uint64_t, Width> starts{};
    uint64_t start = token.start;
    while (start) {
      const unsigned bit = std::countr_zero(start);
      for (size_t lane = 0; lane < Width; ++lane) starts[lane] |= planes[lane][bit];
      start &= start - 1;
    }
    for (size_t lane = 0; lane < Width; ++lane) hits[lane] |= zero[lane] | (one[lane] & starts[lane]);
  }
  return hits;
}

inline uint64_t fit_bits(const uint64_t* planes, uint32_t n, const Token* const* toks, size_t nt) {
  const uint64_t hits = fit_blocks<1>({planes}, toks, nt)[0];
  return n == 64 ? hits : hits & ((uint64_t(1) << n) - 1);
}

}  // namespace fplussearch::detail
