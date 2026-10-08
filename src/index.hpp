#pragma once

#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <functional>
#include <string_view>
#include <vector>

namespace fplussearch {

enum Category : uint8_t { CatNone, CatVideo, CatAudio, CatImage, CatDoc, CatArchive, CatCode, CatFont };

constexpr uint8_t kFlagHidden = 1;  // UF_HIDDEN, set by the Finder
constexpr uint8_t kFlagLink = 2;    // a symbolic link

// Readable zero bytes after every name section so SIMD loads may overrun.
constexpr size_t kPad = 64;
constexpr uint32_t kBlock = 64;  // names per block

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
  Span<uint32_t> code_entry;        // source files for the symbol index, ascending
  Span<uint64_t> code_mtime;        // per source file, nanoseconds since the epoch
  Span<uint32_t> mtime;             // per entry, seconds since the epoch
  Span<uint8_t> flags;              // per entry, kFlag*
  Span<int8_t> dir_prior;           // per directory: how likely what's under it is wanted
  Span<uint32_t> dir_order;         // directories in depth-first preorder
  Span<uint32_t> dir_pre;           // per directory, its position in dir_order
  Span<uint32_t> dir_end;           // per directory, end of its subtree in dir_order
  Span<uint32_t> child_start;       // per directory + 1: its range in children
  Span<uint32_t> children;          // every entry but the root, grouped by parent, ascending
  uint64_t build_id = 0;  // identifies this build to the symbol index
  uint64_t event_id = 0;  // FSEvents id the index is current up to; 0 = unknown
  std::shared_ptr<const void> backing;
  size_t bytes = 0;

  size_t count() const { return n; }
  bool is_dir(uint32_t e) const { return e < dirs; }
  Span<uint32_t> kids(uint32_t d) const { return {children.p + child_start[d], child_start[d + 1] - child_start[d]}; }
  // The child of directory `d` named `name`, or UINT32_MAX. With
  // `any_case`, a name differing only in case also matches (APFS ignores
  // case by default, so a typed path may not match the stored case).
  uint32_t child(uint32_t d, std::string_view name, bool any_case = false) const;
  // The entry at an absolute path, or UINT32_MAX.
  uint32_t lookup(std::string_view path, bool any_case = false) const;
  // Entry e's name, pointing into the index or, for packed hex names, into
  // `buf` (at least 288 bytes).
  std::string_view name_view(uint32_t e, char* buf) const;
  // Whether entry `e` is somewhere under directory `d`.
  bool under(uint32_t e, uint32_t d) const {
    if (e == 0) return false;
    const uint32_t p = dir_pre[parent(e)];
    return p >= dir_pre[d] && p < dir_end[d];
  }
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
  std::atomic<uint64_t> source_files{0};  // read by the symbol pass; nonzero once it starts
};

// Source files larger than this are not scanned for symbols.
constexpr uint64_t kMaxSourceSize = uint64_t(1) << 20;


// Scans `root`, writes the index to `cache_file` and maps it back (or keeps
// it in memory if the file cannot be written), then updates the symbol index
// next to it. `background` lowers the threads' QoS so they don't compete
// with search.
// `on_names` gets the name index as soon as it is saved, before the symbol
// index is built.
Index build_index(const std::string& root, unsigned threads, ScanProgress* progress, bool background,
                  const std::string& cache_file, const std::function<void(const Index&)>& on_names = {});

// One entry of a directory listing.
struct Listed {
  std::string name;
  bool dir;
  uint8_t kind;     // files: size class and Category, as in Index::file_kind
  uint64_t size;    // files
  uint64_t mtime;   // nanoseconds since the epoch
  uint8_t flags;    // kFlag*
  bool regular;     // a regular file (not a link, socket, ...)
};

// Lists one directory; false if it cannot be opened.
bool list_dir(const std::string& path, std::vector<Listed>& out);

// Whether this process may read everything (Full Disk Access).
bool has_full_disk_access();

// Directories the scan skips under `root`.
std::vector<std::string> excluded_dirs(const std::string& root);

// Directories whose contents changed since an index was built.
struct Changes {
  std::vector<std::string> dirs;   // relist these
  std::vector<std::string> trees;  // rescan these whole subtrees
  uint64_t event_id = 0;           // the index is current up to this event
};

// Like build_index, but reads only changed directories (and directories new
// to the index) from disk and copies the rest from `old`.
Index refresh_index(const Index& old, const Changes& changes, unsigned threads, ScanProgress* progress,
                    bool background, const std::string& cache_file);
bool load_index(Index& ix, const std::string& file);
uint8_t categorize(std::string_view name);

// Decodes the packed hex name at `p` into `out`, which needs 32 bytes of slack.
// Returns the bytes consumed from `p`; sets `len` to the name length.
size_t unpack_hex(const char* p, char* out, size_t& len);

}  // namespace fplussearch
