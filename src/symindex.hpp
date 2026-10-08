#pragma once

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "index.hpp"
#include "symbols.hpp"

namespace fplussearch {

// Which files define each symbol name. Built against one file index build;
// occurrences are that build's entry ids.
struct SymbolIndex {
  uint64_t build_id = 0;
  Section names;             // sorted names; per name, the files defining it
  Span<uint32_t> occ;        // file entry per (name, file), grouped by name
  Span<uint64_t> file_fp;    // per scanned source file: fingerprint of path, size, mtime
  Span<uint32_t> file_entry; // per scanned source file: its entry, ascending
  std::shared_ptr<const void> backing;
  size_t bytes = 0;

  std::string_view symbol(uint32_t occurrence) const;
  std::string_view name(uint32_t u) const;
};

std::string symbol_file(const std::string& cache_file);

// Loads the symbol index if it was built against `ix`.
bool load_symbols(SymbolIndex& sx, const std::string& file, const Index& ix);

// Rebuilds the symbol index for `ix`, re-reading only source files whose
// path, size or modification time changed since the previous one.
void build_symbols(const Index& ix, const CodeFile* code, size_t code_count, const std::string& file,
                   ScanProgress* progress, bool background);

struct SymbolLocation {
  bool found = false;
  uint32_t line = 0;
  SymKind kind = SymKind::Function;
};

// Re-reads the file to find where `name` is defined (not stored in the index).
SymbolLocation locate_symbol(const std::string& path, std::string_view name);

}  // namespace fplussearch
