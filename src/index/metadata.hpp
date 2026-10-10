#pragma once

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <string_view>

namespace fplussearch {

enum Category : uint8_t { CatNone, CatVideo, CatAudio, CatImage, CatDoc, CatArchive, CatCode, CatFont };

constexpr uint8_t kFlagHidden = 1;  // UF_HIDDEN, set by the Finder
constexpr uint8_t kFlagLink = 2;    // a symbolic link

inline constexpr std::array<uint8_t, 256> kFold = [] {
  std::array<uint8_t, 256> t{};
  for (int i = 0; i < 256; ++i) t[i] = (i >= 'A' && i <= 'Z') ? uint8_t(i + 32) : uint8_t(i);
  return t;
}();

// A file's kind byte holds its size class (high nibble) and Category (low
// nibble). Classes step by 4x from 256 bytes, so most size filters are decided
// from the kind byte alone and only boundary classes read the exact size.
inline int size_class(uint64_t s) {
  return s < 256 ? 0 : std::min(15, (std::bit_width(s) - 9) / 2 + 1);
}
inline uint64_t class_lo(int c) { return c == 0 ? 0 : uint64_t(1) << (2 * c + 6); }
inline uint64_t class_hi(int c) { return c == 15 ? UINT64_MAX : class_lo(c + 1) - 1; }

// Which character classes a name contains, one bit each (letters, digits,
// '.', '-'/'_', ' ', non-ASCII, other). A query token can only match a name
// whose mask holds all of the token's classes.
inline uint64_t char_bit(uint8_t b) {
  if (b >= 'a' && b <= 'z') return uint64_t(1) << (b - 'a');
  if (b >= 'A' && b <= 'Z') return uint64_t(1) << (b - 'A');
  if (b >= '0' && b <= '9') return uint64_t(1) << (26 + b - '0');
  if (b == '.') return uint64_t(1) << 36;
  if (b == '-' || b == '_') return uint64_t(1) << 37;
  if (b == ' ') return uint64_t(1) << 38;
  if (b >= 0x80) return uint64_t(1) << 39;
  return uint64_t(1) << 40;
}

// Bit for a word starting with `b` (bits 41-63): where a typo match may start.
inline uint64_t start_bit(uint8_t b) { return uint64_t(1) << (41 + kFold[b] % 23); }

// char_bit of every byte, plus start_bit of the name's first byte (past a
// leading dot) and of each space-separated word.
inline uint64_t name_mask(std::string_view s) {
  uint64_t m = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    const uint8_t b = uint8_t(s[i]);
    m |= char_bit(b);
    if (i > 0 && s[i - 1] == ' ') m |= start_bit(b);
  }
  const size_t off = s.size() > 1 && s[0] == '.';
  if (off < s.size()) m |= start_bit(uint8_t(s[off]));
  return m;
}

uint8_t categorize(std::string_view name);

}  // namespace fplussearch
