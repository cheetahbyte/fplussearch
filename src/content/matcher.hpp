#pragma once

#include <bit>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#include "content/content_index.hpp"
#include "content/grams.hpp"
#include "search/regex.hpp"

namespace fplussearch::content_detail {

#if defined(__ARM_NEON)
inline uint64_t lane_bits(uint8x16_t eq) {
  return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0);
}
#endif

// First case-insensitive match of the folded `needle` in [from, end), or npos.
inline size_t find_ci(std::string_view hay, size_t from, std::string_view needle) {
  const size_t k = needle.size();
  if (k == 0 || hay.size() < k || from > hay.size() - k) return std::string_view::npos;
  const size_t last = hay.size() - k;
  const uint8_t f = uint8_t(needle[0]), l = uint8_t(needle[k - 1]);
  const uint8_t fm = f >= 'a' && f <= 'z' ? 0x20 : 0, lm = l >= 'a' && l <= 'z' ? 0x20 : 0;
  auto at = [&](size_t i) {
    for (size_t j = 0; j < k; ++j)
      if (fold(uint8_t(hay[i + j])) != uint8_t(needle[j])) return false;
    return true;
  };
  size_t i = from;
#if defined(__ARM_NEON)
  const uint8x16_t F = vdupq_n_u8(f), L = vdupq_n_u8(l), FM = vdupq_n_u8(fm), LM = vdupq_n_u8(lm);
  const size_t end_off = k - 1;
  for (; i <= last && last - i >= 15; i += 16) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(hay.data() + i);
    const uint8x16_t eq = vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a), FM), F), vceqq_u8(vorrq_u8(vld1q_u8(a + end_off), LM), L));
    uint64_t bits = lane_bits(eq);
    while (bits) {
      const size_t offset = size_t(std::countr_zero(bits) >> 2);
      if (k == 2 || at(i + offset)) return i + offset;
      bits &= ~(uint64_t(0xf) << (offset * 4));
    }
  }
  if (last >= 15 && i <= last) {
    const size_t j = last - 15;
    const uint8_t* a = reinterpret_cast<const uint8_t*>(hay.data() + j);
    const uint8x16_t eq = vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a), FM), F), vceqq_u8(vorrq_u8(vld1q_u8(a + end_off), LM), L));
    uint64_t bits = lane_bits(eq) & (~uint64_t(0) << (4 * (i - j)));
    while (bits) {
      const size_t offset = size_t(std::countr_zero(bits) >> 2);
      if (k == 2 || at(j + offset)) return j + offset;
      bits &= ~(uint64_t(0xf) << (offset * 4));
    }
    return std::string_view::npos;
  }
#endif
  for (; i <= last; ++i)
    if ((uint8_t(hay[i]) | fm) == f && (uint8_t(hay[i + k - 1]) | lm) == l && at(i)) return i;
  return std::string_view::npos;
}

// A compiled pattern: literal (smart-case) or regex.
struct Matcher {
  bool regex = false;
  bool caseless = true;
  std::string needle;  // literal; folded when caseless
  std::shared_ptr<const Regex> re;

  bool find(std::string_view s, size_t from, size_t& a, size_t& b) const {
    if (regex) return re->find(s, from, a, b);
    size_t p;
    if (caseless) {
      p = find_ci(s, from, needle);
    } else {
      const void* r = from <= s.size() ? memmem(s.data() + from, s.size() - from, needle.data(), needle.size()) : nullptr;
      p = r ? size_t(static_cast<const char*>(r) - s.data()) : std::string_view::npos;
    }
    if (p == std::string_view::npos) return false;
    a = p, b = p + needle.size();
    return true;
  }
};

bool make_matcher(const GrepOptions& o, Matcher& m, std::string& error);
bool match_file(const Matcher& m, const std::string& path, size_t per_file, BigVec<char>& buf, FileMatches& out);

}  // namespace fplussearch::content_detail
