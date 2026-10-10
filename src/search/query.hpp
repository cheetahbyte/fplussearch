#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "search/regex.hpp"

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

}  // namespace fplussearch
