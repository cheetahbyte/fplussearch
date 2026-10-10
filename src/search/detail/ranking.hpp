#pragma once

#include <algorithm>
#include <array>
#include <functional>
#include <vector>

#include "index/index.hpp"
#include "search/detail/scoring.hpp"
#include "storage/mapped_array.hpp"

namespace fplussearch::detail {

constexpr uint8_t kNameOk = 1;   // passes as a match on its own
constexpr uint8_t kNameDot = 2;  // starts with '.'
constexpr uint8_t kNameApp = 4;  // ends in .app
constexpr uint8_t kNameNeg = 8;  // a negated token matches it

struct NameHit {
  int16_t score = 0;
  uint8_t bits = 0;  // positive tokens the name matches
  uint8_t flags = 0;
  std::array<int16_t, 4> best{};  // per-token score, first 4 tokens
};

// A directory's own contribution to the entries under it.
struct DirHit {
  uint8_t bits = 0;
  bool neg = false;
  std::array<int16_t, 4> best{};
};

inline uint8_t name_flags(std::string_view name) {
  uint8_t f = 0;
  if (name.starts_with('.')) f |= kNameDot;
  if (name.ends_with(".app")) f |= kNameApp;
  return f;
}

// Small per-entry nudges on top of match quality and the location prior.
inline int32_t rank_tweaks(uint8_t nflags, bool dir_or_link, uint8_t eflags, uint32_t mtime, uint32_t now) {
  int32_t s = 0;
  if (nflags & kNameDot) s -= 8;
  if (eflags & kFlagHidden) s -= 8;
  if (dir_or_link && (nflags & kNameApp)) s += 25;
  const uint32_t age = now > mtime ? now - mtime : 0;
  s += age <= 86400 ? 10 : age <= 604800 ? 7 : age <= 2592000 ? 4 : age <= 31536000 ? 1 : 0;
  return s;
}

// Score, then folder order (depth-first, so roughly path order), then entry.
using Key = unsigned __int128;
inline Key rank_key(int32_t score, uint32_t folder, uint32_t e) {
  const uint64_t hi = (uint64_t(int64_t(score) - int64_t(INT32_MIN)) << 32) | uint32_t(~folder);
  return (Key(hi) << 32) | uint32_t(~e);
}
inline uint32_t key_entry(Key k) { return ~uint32_t(k); }
inline int32_t key_score(Key k) { return int32_t(int64_t(uint64_t(k >> 64) & 0xffffffff) + INT32_MIN); }

// The most an entry adds to its name's score: the best folder prior plus
// every rank tweak.
constexpr int32_t kMaxEntryBonus = 60 + 25 + 10;

inline int32_t token_bound(const Token& t, std::string_view name) {
  const int32_t len = length_cost(name);
  switch (t.mode) {
    case Mode::Exact: return 70 - len;
    case Mode::Prefix: return 60 - len;
    case Mode::Suffix: return 50 - len;
    case Mode::Fuzzy: break;
  }
  const size_t off = name.size() > 1 && name[0] == '.';
  const int32_t chars = 32 + 24 * int32_t(t.text.size() - 1);
  const bool whole_typo = takes_typos(t) && name.size() + 1 >= t.text.size() && name.size() <= t.text.size() + 1;
  const int32_t placed = whole_typo || (off < name.size() && fold(uint8_t(name[off])) == uint8_t(t.text[0])) ? 100 : 0;
  return chars + placed - len;
}

// The best k keys: candidates above the floor collect in a buffer that is
// cut back to k now and then, cheaper than a heap when most entries match.
struct TopK {
  size_t k;
  BigVec<Key> buf;
  Key floor = 0;
  explicit TopK(size_t k_) : k(k_), floor(k_ == 0 ? ~Key(0) : 0) {}
  bool full() const { return k > 0 && floor != 0; }
  void push(Key key) {
    if (key <= floor) return;
    buf.push_back(key);
    if (buf.size() >= std::max<size_t>(2 * k, 64)) cut();
  }
  void cut() {
    if (buf.size() <= k) return;
    std::nth_element(buf.begin(), buf.begin() + long(k - 1), buf.end(), std::greater<>());
    buf.resize(k);
    floor = buf[k - 1];
  }
};

struct alignas(128) Chunk {
  uint64_t count = 0;
  TopK top{0};
  std::vector<uint32_t> all;  // each_match: every hit, unranked
};

}  // namespace fplussearch::detail
