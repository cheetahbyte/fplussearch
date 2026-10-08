#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "index.hpp"
#include "pool.hpp"
#include "symindex.hpp"

namespace fplussearch {

// Allowed entry kinds: one bit per file Category, plus directories.
constexpr uint32_t kTypeFiles = 0xffff;
constexpr uint32_t kTypeDir = 1u << 16;
constexpr uint32_t kTypeAny = kTypeFiles | kTypeDir;

struct Query {
  std::vector<std::string> terms;  // case-folded name substrings, all must match
  std::vector<std::string> exts;   // case-folded ".ext" suffixes, any may match
  std::vector<std::string> syms;   // case-folded symbol name substrings (sym:), all must match
  uint64_t min_size = 0;
  uint64_t max_size = UINT64_MAX;
  bool has_size = false;
  uint32_t types = kTypeAny;  // type: tokens; values in one token are alternatives
  std::string error;
};

Query parse_query(std::string_view text);

struct Results {
  uint64_t total = 0;
  std::vector<uint32_t> top;  // first `limit` matches: directories, then files, by name
};

class Engine {
 public:
  explicit Engine(unsigned threads) : pool_(threads) {}
  Results search(const Index& ix, const Query& q, size_t limit);
  // Matches symbol definitions; `top` holds occurrence ids into `sx`.
  Results search_symbols(const Index& ix, const SymbolIndex& sx, const Query& q, size_t limit);

 private:
  Pool pool_;
};

}  // namespace fplussearch
