#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "index/metadata.hpp"
#include "index/section.hpp"

namespace fplussearch {

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

}  // namespace fplussearch
