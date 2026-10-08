#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <string_view>

namespace fplussearch {

enum Category : uint8_t { CatNone, CatVideo, CatAudio, CatImage, CatDoc, CatArchive, CatCode };

// Readable zero bytes after every name section so SIMD loads may overrun.
constexpr size_t kPad = 64;
constexpr uint32_t kBlock = 64;  // names per block
constexpr size_t kGramSlots = 256 + 65536;  // folded bytes, then folded byte pairs

inline size_t gram_slot(std::string_view folded) {
  return folded.size() == 1 ? uint8_t(folded[0]) : 256 + (size_t(uint8_t(folded[0])) << 8 | uint8_t(folded[1]));
}

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

template <typename T>
struct Span {
  const T* p = nullptr;
  size_t n = 0;
  const T& operator[](size_t i) const { return p[i]; }
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

  uint32_t names() const { return uint32_t(name_off.n); }
  uint32_t blocks() const { return uint32_t(block_off.n - 1); }
  uint32_t first_entry() const { return block_first[0]; }
  uint32_t end_entry() const { return block_first[block_first.n - 1]; }
  uint32_t count(uint32_t name) const;
};

// Read-only index, usually a memory-mapped cache file. Directories are entries
// [0, dirs) and files [dirs, n); within each, entries sharing a name are
// contiguous and ordered by name. Entry 0 is the root.
struct Index {
  std::string root;
  uint32_t n = 0;
  uint32_t dirs = 0;
  uint32_t parent_bits = 0;
  std::array<Section, 4> sections;  // dir text, dir hex, file text, file hex
  Span<uint8_t> parents;            // parent_bits per entry, packed
  Span<uint8_t> file_kind;          // per file
  Span<uint32_t> file_size;         // per file; UINT32_MAX = see big_sizes
  Span<Overflow> big_sizes;         // entry -> size, sorted
  Span<uint32_t> grams;             // [dirs, files][gram_slot]: entries whose name contains it
  std::shared_ptr<const void> backing;
  size_t bytes = 0;

  size_t count() const { return n; }
  bool is_dir(uint32_t e) const { return e < dirs; }
  uint32_t parent(uint32_t e) const {
    const uint64_t bit = uint64_t(e) * parent_bits;
    uint64_t w;
    std::memcpy(&w, parents.p + (bit >> 3), sizeof w);
    return uint32_t((w >> (bit & 7)) & ((uint64_t(1) << parent_bits) - 1));
  }
  uint64_t size(uint32_t e) const {  // files only
    const uint32_t s = file_size[e - dirs];
    return s != UINT32_MAX ? s : big_size(e);
  }
  uint64_t big_size(uint32_t e) const;
  std::string name(uint32_t e) const;
  std::string path(uint32_t e) const;
};

struct ScanProgress {
  std::atomic<uint64_t> entries{0};
};

// Scans `root`, writes the index to `cache_file` and maps it back (or keeps
// it in memory if the file cannot be written). `background` lowers the scan
// threads' QoS so they don't compete with search.
Index build_index(const std::string& root, unsigned threads, ScanProgress* progress, bool background,
                  const std::string& cache_file);
bool load_index(Index& ix, const std::string& file);
uint8_t categorize(std::string_view name);

// Decodes the packed hex name at `p` into `out`, which needs 32 bytes of slack.
// Returns the bytes consumed from `p`; sets `len` to the name length.
size_t unpack_hex(const char* p, char* out, size_t& len);

}  // namespace fplussearch
