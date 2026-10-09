#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "index.hpp"
#include "pool.hpp"
#include "overlay_storage.hpp"
#include "regex.hpp"
#include "symindex.hpp"

namespace fplussearch {

// Allowed entry kinds: one bit per file Category, plus directories.
constexpr uint32_t kTypeFiles = 0xffff;
constexpr uint32_t kTypeDir = 1u << 16;
constexpr uint32_t kTypeAny = kTypeFiles | kTypeDir;

// Fuzzy matches contiguous text and allows typos; Exact disables typos.
enum class Mode : uint8_t { Fuzzy, Exact, Prefix, Suffix };
enum class Kind : uint8_t { Any, File, Dir, Link };
enum class GrepMode : uint8_t { None, Literal, Regex };

// One word of a query, matched against names (and the folders above them).
struct Token {
  std::string text;  // case-folded
  Mode mode = Mode::Fuzzy;
  bool negate = false;
  uint64_t mask = 0;   // char_bit of every byte
  uint64_t loose = 0;  // classes a one-typo match may lack; 0 = no typos
  uint64_t start = 0;  // start_bit of the first byte when typos are allowed

  // Whether a name with name_mask `m` can match, cleanly or with one typo.
  bool fits(uint64_t m) const {
    const uint64_t miss = mask & ~m;
    return miss == 0 || (((miss & ~loose) | (miss & (miss - 1))) == 0 && (m & start) != 0);
  }
};

struct Query {
  std::vector<Token> tokens;
  std::vector<std::string> exts;  // case-folded, without the dot; any may match
  uint32_t types = kTypeAny;      // type: categories and folders
  Kind kind = Kind::Any;          // kind:
  std::string scope;              // in: (a real path), empty = everywhere
  uint64_t min_size = 0, max_size = UINT64_MAX;
  bool has_size = false;
  uint32_t min_mtime = 0, max_mtime = UINT32_MAX;  // seconds since the epoch
  std::shared_ptr<const Regex> name_re, path_re;   // re:, path:
  size_t limit = 0;                                // limit:, 0 = the caller's
  std::vector<std::string> syms;                   // sym: (case-folded substrings)
  std::string grep;                                // grep:/content: or regex:
  GrepMode grep_mode = GrepMode::None;
  std::string error;

  bool has_tokens() const;
  // Anything that selects entries (a bare query selects nothing).
  bool selects() const;
};

// `home` expands "~" in in: paths.
Query parse_query(std::string_view text, std::string_view home = {});

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

// Whether a file path passes the query's name words, ext:, type: (by
// extension), re: and path: — the filters content search applies to the
// files it reads. Size, date and in: are left to the caller.
bool path_matches(const Query& q, std::string_view path);

// Case-insensitive substring scoring, shared with other match sources.
int32_t fuzzy_score(std::string_view name, std::string_view q);
// Score of one token against a name with name_mask `m`, or INT32_MIN.
int32_t token_score(std::string_view name, uint64_t m, const Token& t);
constexpr int32_t kNoMatch = INT32_MIN;

}  // namespace fplussearch
