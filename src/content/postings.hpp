#pragma once

#include <cstdint>
#include <vector>

namespace fplussearch::content_detail {

inline constexpr uint32_t kBitset = 0x80000000u;  // tri_off flag: the list is a bitset over docs

inline void put_varint(std::vector<uint8_t>& out, uint32_t v) {
  while (v >= 0x80) {
    out.push_back(uint8_t(v | 0x80));
    v >>= 7;
  }
  out.push_back(uint8_t(v));
}

inline const uint8_t* get_varint(const uint8_t* p, uint32_t& v) {
  v = 0;
  for (int s = 0;; s += 7) {
    const uint8_t b = *p++;
    v |= uint32_t(b & 0x7f) << s;
    if (!(b & 0x80) || s >= 28) return p;
  }
}

}  // namespace fplussearch::content_detail
