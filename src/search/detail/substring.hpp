#pragma once

#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

#include "index/index.hpp"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace fplussearch::detail {

inline uint8_t fold(uint8_t b) { return kFold[b]; }

inline bool fold_eq(const char* p, std::string_view needle) {
  for (size_t j = 0; j < needle.size(); ++j)
    if (kFold[uint8_t(p[j])] != uint8_t(needle[j])) return false;
  return true;
}

inline bool contains_fold(std::string_view hay, std::string_view needle) {
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0, last = hay.size() - needle.size(); i <= last; ++i)
    if (fold_eq(hay.data() + i, needle)) return true;
  return false;
}

// First position in [p, end) where the folded needle matches, or end. Bytes
// past `end` may be read (the blob is padded) but never reported.
inline const char* find_fold(const char* p, const char* end, std::string_view needle) {
  const size_t k = needle.size();
  if (end - p < ptrdiff_t(k)) return end;
  const char* limit = end - k + 1;
  const uint8_t first = uint8_t(needle[0]), last = uint8_t(needle[k - 1]);
  // OR-ing 0x20 maps exactly 'A'-'Z' onto 'a'-'z', so letters compare
  // case-insensitively with no false positives; other bytes compare exactly.
  const uint8_t fm = (first >= 'a' && first <= 'z') ? 0x20 : 0;
  const uint8_t lm = (last >= 'a' && last <= 'z') ? 0x20 : 0;
#if defined(__ARM_NEON)
  const uint8x16_t F = vdupq_n_u8(first), L = vdupq_n_u8(last);
  const uint8x16_t FM = vdupq_n_u8(fm), LM = vdupq_n_u8(lm);
  for (; p < limit; p += 32) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(p);
    const uint8_t* b = a + k - 1;
    uint8x16_t e0 = vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a), FM), F), vceqq_u8(vorrq_u8(vld1q_u8(b), LM), L));
    uint8x16_t e1 =
        vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a + 16), FM), F), vceqq_u8(vorrq_u8(vld1q_u8(b + 16), LM), L));
    if (vmaxvq_u8(vorrq_u8(e0, e1)) == 0) continue;
    for (int half = 0; half < 2; ++half) {
      uint8x8_t nib = vshrn_n_u16(vreinterpretq_u16_u8(half ? e1 : e0), 4);
      uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(nib), 0);
      while (mask) {
        int bit = __builtin_ctzll(mask) >> 2;
        const char* c = p + half * 16 + bit;
        if (c >= limit) return end;
        if (fold_eq(c, needle)) return c;
        mask &= ~(0xfull << (bit * 4));
      }
    }
  }
  return end;
#else
  for (; p < limit; ++p)
    if ((uint8_t(p[0]) | fm) == first && (uint8_t(p[k - 1]) | lm) == last && fold_eq(p, needle)) return p;
  return end;
#endif
}

// The other case of a folded query byte, or the byte itself.
inline uint8_t other_case(uint8_t c) { return c >= 'a' && c <= 'z' ? uint8_t(c - 32) : c; }

#if defined(__ARM_NEON)
// Bit 4i..4i+3 set where lane i of an all-ones-or-zero compare is set.
inline uint64_t lane_bits(uint8x16_t eq) { return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0); }
#endif

// First index >= `from` whose byte folds to `c` (an already folded byte).
inline size_t find_folded(std::string_view s, size_t from, uint8_t c) {
  const uint8_t u = other_case(c);
  size_t i = from;
#if defined(__ARM_NEON)
  const uint8x16_t L = vdupq_n_u8(c), U = vdupq_n_u8(u);
  for (; i + 16 <= s.size(); i += 16) {
    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(s.data() + i));
    const uint64_t bits = lane_bits(vorrq_u8(vceqq_u8(v, L), vceqq_u8(v, U)));
    if (bits) return i + size_t(__builtin_ctzll(bits) >> 2);
  }
#endif
  for (; i < s.size(); ++i)
    if (uint8_t(s[i]) == c || uint8_t(s[i]) == u) return i;
  return std::string_view::npos;
}

inline size_t find_ci(std::string_view name, std::string_view q) {
  if (q.size() > name.size()) return std::string_view::npos;
  if (q.empty()) return 0;
  if (q.size() == 1) return find_folded(name, 0, uint8_t(q[0]));
  const size_t last = name.size() - q.size();
  const uint8_t first = uint8_t(q.front()), end = uint8_t(q.back());
  const uint8_t fm = first >= 'a' && first <= 'z' ? 0x20 : 0;
  const uint8_t lm = end >= 'a' && end <= 'z' ? 0x20 : 0;
  size_t i = 0;
#if defined(__ARM_NEON)
  const uint8x16_t F = vdupq_n_u8(first), L = vdupq_n_u8(end);
  const uint8x16_t FM = vdupq_n_u8(fm), LM = vdupq_n_u8(lm);
  const size_t q_end_off = q.size() - 1;
  for (; i <= last && last - i >= 15; i += 16) {
    const auto* p = reinterpret_cast<const uint8_t*>(name.data() + i);
    const uint8x16_t matches = vandq_u8(
        vceqq_u8(vorrq_u8(vld1q_u8(p), FM), F),
        vceqq_u8(vorrq_u8(vld1q_u8(p + q_end_off), LM), L));
    uint64_t bits = lane_bits(matches);
    while (bits) {
      const size_t offset = size_t(std::countr_zero(bits) >> 2);
      if (q.size() == 2 || fold_eq(name.data() + i + offset, q)) return i + offset;
      bits &= ~(uint64_t(0xf) << (offset * 4));
    }
  }
  if (last >= 15 && i <= last) {
    const size_t j = last - 15;
    const auto* p = reinterpret_cast<const uint8_t*>(name.data() + j);
    const uint8x16_t matches = vandq_u8(
        vceqq_u8(vorrq_u8(vld1q_u8(p), FM), F),
        vceqq_u8(vorrq_u8(vld1q_u8(p + q_end_off), LM), L));
    uint64_t bits = lane_bits(matches) & (~uint64_t(0) << (4 * (i - j)));
    while (bits) {
      const size_t offset = size_t(std::countr_zero(bits) >> 2);
      if (q.size() == 2 || fold_eq(name.data() + j + offset, q)) return j + offset;
      bits &= ~(uint64_t(0xf) << (offset * 4));
    }
    return std::string_view::npos;
  }
#endif
  for (; i <= last; ++i)
    if ((uint8_t(name[i]) | fm) == first && (uint8_t(name[i + q.size() - 1]) | lm) == end &&
        (q.size() == 2 || fold_eq(name.data() + i, q))) return i;
  return std::string_view::npos;
}

}  // namespace fplussearch::detail
