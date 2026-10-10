#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace fplussearch {

// Readable zero bytes after every name section so SIMD loads may overrun.
constexpr size_t kPad = 64;
constexpr uint32_t kBlock = 64;  // names per block

template <typename T>
struct Span {
  const T* p = nullptr;
  size_t n = 0;
  const T& operator[](size_t i) const { return p[i]; }
  const T* begin() const { return p; }
  const T* end() const { return p + n; }
};

struct Overflow {
  uint32_t key;
  uint32_t unused;
  uint64_t value;
};

// A sorted run of names, in blocks of kBlock. Text names are NUL-terminated;
// hex names (lowercase hashes, common in package stores) are packed two digits
// per byte as [length][digits].
struct Section {
  bool hex = false;
  Span<char> bytes;
  Span<uint32_t> block_off;    // blocks + 1, into bytes
  Span<uint32_t> block_first;  // blocks + 1, first entry of each block's first name
  Span<uint16_t> name_off;     // per name, offset within its block
  Span<uint8_t> counts;        // per name, entries with that name; 255 = see overflow
  Span<Overflow> overflow;     // name -> count, sorted
  Span<uint64_t> mask;         // per name, name_mask
  std::shared_ptr<const std::vector<std::array<uint64_t, kBlock>>> mask_planes;
  std::shared_ptr<const std::vector<uint32_t>> name_first;  // first entry per name

  uint32_t names() const { return uint32_t(name_off.n); }
  uint32_t blocks() const { return uint32_t(block_off.n - 1); }
  uint32_t first_entry() const { return block_first[0]; }
  uint32_t end_entry() const { return block_first[block_first.n - 1]; }
  inline uint32_t count(uint32_t name) const {
    const uint8_t c = counts[name];
    if (c != 255) [[likely]] return c;
    return count_slow(name);
  }
  uint32_t count_slow(uint32_t name) const;
};

// Decodes the packed hex name at `p` into `out`, which needs 32 bytes of slack.
// Returns the bytes consumed from `p`; sets `len` to the name length.
size_t unpack_hex(const char* p, char* out, size_t& len);

}  // namespace fplussearch
