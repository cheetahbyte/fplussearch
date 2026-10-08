#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "index.hpp"
#include "pool.hpp"

namespace fplussearch {

enum class Want : uint8_t { Any, Dirs, Files };

struct Query {
  std::vector<std::string> terms;  // case-folded name substrings, all must match
  std::vector<std::string> exts;   // case-folded ".ext" suffixes, any may match
  uint64_t min_size = 0;
  uint64_t max_size = UINT64_MAX;
  bool has_size = false;
  uint16_t cat_mask = 0;  // bit per Category, 0 = any
  Want want = Want::Any;
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

 private:
  Pool pool_;
};

}  // namespace fplussearch
