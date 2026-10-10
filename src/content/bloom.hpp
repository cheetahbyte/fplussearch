#pragma once

#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

#include "index/index.hpp"

namespace fplussearch::content_detail {

struct Bloom {
  std::vector<uint64_t> short_bits, long_bits;
};
struct BloomView { std::span<const uint64_t> short_bits, long_bits; uint64_t mtime_ns = 0, ctime_ns = 0; };

inline uint32_t gram_hash(const char* p, size_t n) {
  uint32_t h = 2166136261u;
  for (size_t i = 0; i < n; ++i) h = (h ^ kFold[uint8_t(p[i])]) * 16777619u;
  return h;
}

inline void bloom_set(std::vector<uint64_t>& b, uint32_t h) {
  const size_t words = b.size();
  const size_t a = (uint64_t(h) * words * 64) >> 32;
  const size_t c = (uint64_t(h * 0x9e3779b1u) * words * 64) >> 32;
  b[a / 64] |= uint64_t(1) << (a % 64);
  b[c / 64] |= uint64_t(1) << (c % 64);
}

inline bool bloom_has(std::span<const uint64_t> b, uint32_t h) {
  const size_t words = b.size();
  if (!words) return true;
  const size_t a = (uint64_t(h) * words * 64) >> 32;
  const size_t c = (uint64_t(h * 0x9e3779b1u) * words * 64) >> 32;
  return (b[a / 64] >> (a % 64) & 1) && (b[c / 64] >> (c % 64) & 1);
}

void make_bloom(std::string_view s, Bloom& b);

}  // namespace fplussearch::content_detail
