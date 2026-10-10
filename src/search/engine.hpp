#pragma once

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

#include "index/index.hpp"
#include "live/overlay_storage.hpp"
#include "search/query.hpp"
#include "support/pool.hpp"

namespace fplussearch {

struct SymbolIndex;

// An entry the index doesn't have yet: the live overlay of changes.
struct Extra {
  std::string path;
  bool dir = false;
  uint8_t kind = 0;  // files: size class and Category, as Index::file_kind
  uint64_t size = 0;
  uint32_t mtime = 0;  // seconds since the epoch
  uint8_t flags = 0;   // kFlag*
  uint64_t mask = 0;   // name_mask of its name
};

// Changes on top of an index: entries gone since, and entries added.
struct Overlay {
  DeadView dead;  // one bit per index entry
  std::vector<const Extra*> extras;
};

struct Results {
  uint64_t total = 0;
  std::vector<uint32_t> top;            // best first; UINT32_MAX for an overlay entry
  std::vector<int32_t> score;           // per top entry (name searches)
  std::vector<const Extra*> extra;      // per top entry: the overlay entry, or null
};

class Engine {
 public:
  explicit Engine(unsigned threads) : pool_(threads) {}
  // Ranked name search: entries matching every token (in their own name or
  // a folder above them) and every filter, best `limit` first.
  Results search(const Index& ix, const Query& q, size_t limit, const Overlay* overlay = nullptr);
  // Matches symbol definitions; `top` holds occurrence ids into `sx`.
  Results search_symbols(const Index& ix, const SymbolIndex& sx, const Query& q, size_t limit);
  // Calls `take(entry)` for every file matching the query's name tokens and
  // filters, from several threads at once.
  template <typename Take>
  void each_file(const Index& ix, const Query& q, Take&& take);
  Pool& pool() { return pool_; }

 private:
  void each_match(const Index& ix, const Query& q, bool files_only, const std::function<void(uint32_t)>& take);
  Pool pool_;
};

template <typename Take>
void Engine::each_file(const Index& ix, const Query& q, Take&& take) {
  each_match(ix, q, true, [&](uint32_t e) { take(e); });
}

}  // namespace fplussearch
