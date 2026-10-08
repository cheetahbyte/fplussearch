#include "search.hpp"

#include "store.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <ctime>
#include <functional>
#include <unordered_map>
#include <sys/stat.h>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace fplussearch {

namespace {

// ---------------------------------------------------------------- matching

uint8_t fold(uint8_t b) { return kFold[b]; }

bool fold_eq(const char* p, std::string_view needle) {
  for (size_t j = 0; j < needle.size(); ++j)
    if (kFold[uint8_t(p[j])] != uint8_t(needle[j])) return false;
  return true;
}

bool contains_fold(std::string_view hay, std::string_view needle) {
  if (needle.size() > hay.size()) return false;
  for (size_t i = 0, last = hay.size() - needle.size(); i <= last; ++i)
    if (fold_eq(hay.data() + i, needle)) return true;
  return false;
}

// First position in [p, end) where the folded needle matches, or end. Bytes
// past `end` may be read (the blob is padded) but never reported.
const char* find_fold(const char* p, const char* end, std::string_view needle) {
  const size_t k = needle.size();
  if (end - p < ptrdiff_t(k)) return end;
  const char* limit = end - k + 1;
  const uint8_t first = uint8_t(needle[0]), last = uint8_t(needle[k - 1]);
  // OR-ing 0x20 maps exactly 'A'-'Z' onto 'a'-'z', so letters compare
  // case-insensitively with no false positives; other bytes compare exactly.
  const uint8_t fm = (first >= 'a' && first <= 'z') ? 0x20 : 0;
  const uint8_t lm = (last >= 'a' && last <= 'z') ? 0x20 : 0;
#if defined(__ARM_NEON)
  const uint8x16_t F = vdupq_n_u8(first), L = vdupq_n_u8(last);
  const uint8x16_t FM = vdupq_n_u8(fm), LM = vdupq_n_u8(lm);
  for (; p < limit; p += 32) {
    const uint8_t* a = reinterpret_cast<const uint8_t*>(p);
    const uint8_t* b = a + k - 1;
    uint8x16_t e0 = vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a), FM), F), vceqq_u8(vorrq_u8(vld1q_u8(b), LM), L));
    uint8x16_t e1 =
        vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a + 16), FM), F), vceqq_u8(vorrq_u8(vld1q_u8(b + 16), LM), L));
    if (vmaxvq_u8(vorrq_u8(e0, e1)) == 0) continue;
    for (int half = 0; half < 2; ++half) {
      uint8x8_t nib = vshrn_n_u16(vreinterpretq_u16_u8(half ? e1 : e0), 4);
      uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(nib), 0);
      while (mask) {
        int bit = __builtin_ctzll(mask) >> 2;
        const char* c = p + half * 16 + bit;
        if (c >= limit) return end;
        if (fold_eq(c, needle)) return c;
        mask &= ~(0xfull << (bit * 4));
      }
    }
  }
  return end;
#else
  for (; p < limit; ++p)
    if ((uint8_t(p[0]) | fm) == first && (uint8_t(p[k - 1]) | lm) == last && fold_eq(p, needle)) return p;
  return end;
#endif
}

enum class Class : uint8_t { Lower, Upper, Digit, Delim, Other };

Class char_class(uint8_t b) {
  if (b >= 'a' && b <= 'z') return Class::Lower;
  if (b >= 'A' && b <= 'Z') return Class::Upper;
  if (b >= '0' && b <= '9') return Class::Digit;
  switch (b) {
    case ' ': case '_': case '-': case '.': case '/': case '(': case ')': case '[': case ']': case ',': case '+':
    case '@':
      return Class::Delim;
    default:
      return Class::Other;
  }
}

constexpr int32_t kScoreMatch = 16, kGapStart = -3, kGapExt = -1;
constexpr int32_t kBonusBoundary = 8, kBonusCamel = 7, kBonusConsec = 4;
constexpr size_t kTypoMinLen = 5;  // fuzzy words this long forgive one typo
constexpr int32_t kTypoCost = 60;  // so clean matches of the same quality rank first

int32_t bonus(Class prev, Class cur) {
  if (prev == Class::Delim && cur != Class::Delim) return kBonusBoundary;
  if ((prev == Class::Lower && cur == Class::Upper) ||
      ((prev == Class::Lower || prev == Class::Upper) && cur == Class::Digit))
    return kBonusCamel;
  return 0;
}

// The other case of a folded query byte, or the byte itself.
uint8_t other_case(uint8_t c) { return c >= 'a' && c <= 'z' ? uint8_t(c - 32) : c; }

#if defined(__ARM_NEON)
// Bit 4i..4i+3 set where lane i of an all-ones-or-zero compare is set.
uint64_t lane_bits(uint8x16_t eq) { return vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(eq), 4)), 0); }
#endif

// First index >= `from` whose byte folds to `c` (an already folded byte).
size_t find_folded(std::string_view s, size_t from, uint8_t c) {
  const uint8_t u = other_case(c);
  size_t i = from;
#if defined(__ARM_NEON)
  const uint8x16_t L = vdupq_n_u8(c), U = vdupq_n_u8(u);
  for (; i + 16 <= s.size(); i += 16) {
    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(s.data() + i));
    const uint64_t bits = lane_bits(vorrq_u8(vceqq_u8(v, L), vceqq_u8(v, U)));
    if (bits) return i + size_t(__builtin_ctzll(bits) >> 2);
  }
#endif
  for (; i < s.size(); ++i)
    if (uint8_t(s[i]) == c || uint8_t(s[i]) == u) return i;
  return std::string_view::npos;
}

// Last index < `before` whose byte folds to c.
size_t rfind_folded(std::string_view s, size_t before, uint8_t c) {
  const uint8_t u = other_case(c);
  size_t i = before;
#if defined(__ARM_NEON)
  const uint8x16_t L = vdupq_n_u8(c), U = vdupq_n_u8(u);
  for (; i >= 16; i -= 16) {
    const uint8x16_t v = vld1q_u8(reinterpret_cast<const uint8_t*>(s.data() + i - 16));
    const uint64_t bits = lane_bits(vorrq_u8(vceqq_u8(v, L), vceqq_u8(v, U)));
    if (bits) return i - 16 + size_t((63 - __builtin_clzll(bits)) >> 2);
  }
#endif
  while (i-- > 0)
    if (uint8_t(s[i]) == c || uint8_t(s[i]) == u) return i;
  return std::string_view::npos;
}

int32_t length_cost(std::string_view name) { return int32_t(std::min<size_t>(name.size(), 80)) / 3; }

// Where the name's stem ends: before its extension, ignoring a leading dot.
size_t stem_end(std::string_view name, size_t off) {
  const size_t dot = name.rfind('.');
  return dot != std::string_view::npos && dot > off ? dot : name.size();
}

int32_t single_score(std::string_view name, size_t i, int32_t cap) {
  const Class prev = i == 0 ? Class::Delim : char_class(uint8_t(name[i - 1]));
  int32_t score = kScoreMatch + bonus(prev, char_class(uint8_t(name[i]))) * 2;
  const size_t off = name.size() > 1 && name[0] == '.';
  if (i == off) score += std::min(cap, i + 1 == name.size() ? 100 : i + 1 == stem_end(name, off) ? 80 : 30);
  return score - length_cost(name);
}

// Where the leftmost match of `q` as a subsequence of `name` ends, or npos.
// One pass, one compare per byte: letters fold with | 0x20, which maps
// exactly 'A'-'Z' onto 'a'-'z'.
size_t subseq_end(std::string_view name, std::string_view q) {
  size_t j = 0;
  uint8_t want = uint8_t(q[0]), set = want >= 'a' && want <= 'z' ? 0x20 : 0;
  for (size_t i = 0; i < name.size(); ++i) {
    if ((uint8_t(name[i]) | set) != want) continue;
    if (++j == q.size()) return i;
    want = uint8_t(q[j]);
    set = want >= 'a' && want <= 'z' ? 0x20 : 0;
  }
  return std::string_view::npos;
}

// fzf-v1 style: the leftmost-ending match, shrunk from the right, scored
// with boundary, camelCase and consecutive bonuses; whole-name, stem and
// prefix matches get up to `cap` more.
int32_t fuzzy_capped(std::string_view name, std::string_view q, int32_t cap) {
  const size_t end = subseq_end(name, q);
  if (end == std::string_view::npos) return kNoMatch;
  if (q.size() == 1) return single_score(name, end, cap);
  size_t start = end + 1;
  for (size_t k = q.size(); k-- > 0;) {
    start = rfind_folded(name, start, uint8_t(q[k]));
    if (start == std::string_view::npos) return kNoMatch;
  }
  int32_t score = 0, first_bonus = 0;
  size_t at = start;
  for (size_t k = 0; k < q.size(); ++k) {
    bool run = false;
    if (k > 0) {
      const size_t last = at;
      at = find_folded(name, last + 1, uint8_t(q[k]));
      if (at == std::string_view::npos || at > end) return kNoMatch;
      run = at == last + 1;
      if (!run) score += kGapStart + int32_t(at - last - 2) * kGapExt;
    }
    const Class prev = at == 0 ? Class::Delim : char_class(uint8_t(name[at - 1]));
    int32_t b = bonus(prev, char_class(uint8_t(name[at])));
    if (run) {
      if (b >= kBonusBoundary && b > first_bonus) first_bonus = b;
      b = std::max({b, first_bonus, kBonusConsec});
    } else {
      first_bonus = b;
    }
    score += kScoreMatch + (k == 0 ? b * 2 : b);
  }
  // Whole-name and stem matches are what people mean most of the time. A
  // leading dot doesn't count: "zshrc" means ~/.zshrc.
  const size_t off = name.size() > 1 && name[0] == '.';
  const bool contiguous = end + 1 - start == q.size();
  int32_t placed = 0;
  if (start == off && contiguous) placed = end + 1 == name.size() ? 100 : end + 1 == stem_end(name, off) ? 80 : 30;
  return score + std::min(placed, cap) - length_cost(name);
}

bool starts_fold(std::string_view w, std::string_view q) {
  return w.size() >= q.size() && fold_eq(w.data(), q);
}

bool is_digit(uint8_t c) { return c >= '0' && c <= '9'; }

// How long a prefix of `w` the query `q` spells with exactly one edit (a
// wrong, extra, missing or swapped letter), or 0. Digits are never edited:
// "hat_18" is another file than "hat_98", not a typo of it.
size_t one_edit_prefix(std::string_view w, std::string_view q) {
  size_t i = 0;
  while (i < q.size() && i < w.size() && fold(uint8_t(w[i])) == uint8_t(q[i])) ++i;
  if (i == q.size()) return 0;  // a clean prefix, not a typo
  if (is_digit(uint8_t(q[i])) || (i < w.size() && is_digit(uint8_t(w[i])))) return 0;
  const std::string_view rest = q.substr(i + 1);
  const std::string_view after = i + 1 <= w.size() ? w.substr(i + 1) : std::string_view();
  if (i + 1 < q.size() && i + 1 < w.size() && fold(uint8_t(w[i])) == uint8_t(q[i + 1]) &&
      fold(uint8_t(w[i + 1])) == uint8_t(q[i]) && starts_fold(w.substr(i + 2), q.substr(i + 2)))
    return q.size();  // swapped
  if (i < w.size() && starts_fold(after, rest)) return q.size();  // wrong letter
  if (starts_fold(w.substr(i), rest)) return q.size() - 1;      // extra letter in q
  if (i < w.size() && starts_fold(after, q.substr(i))) return q.size() + 1;  // missing letter in q
  return 0;
}

int32_t typo_at(std::string_view name, size_t s, std::string_view q) {
  if (s >= name.size() || fold(uint8_t(name[s])) != uint8_t(q[0])) return kNoMatch;
  const size_t n = one_edit_prefix(name.substr(s), q);
  if (n == 0 || n > 128) return kNoMatch;
  char fixed[128];
  for (size_t i = 0; i < n; ++i) fixed[i] = char(fold(uint8_t(name[s + i])));
  const int32_t sc = fuzzy_capped(name, std::string_view(fixed, n), 30);
  return sc == kNoMatch ? kNoMatch : sc - kTypoCost;
}

// Best score for `q` read with one typo at the start of the name or of a
// word in it, scored as if typed right, minus kTypoCost and never placed
// above a prefix ("manif" is "manifest" being typed, not a typo of "manic").
int32_t typo_score(std::string_view name, uint64_t m, std::string_view q) {
  int32_t best = typo_at(name, name.size() > 1 && name[0] == '.', q);
  if (m & char_bit(' '))
    for (size_t i = 0; i + 1 < name.size(); ++i)
      if (name[i] == ' ') best = std::max(best, typo_at(name, i + 1, q));
  return best;
}

// Whether `name` is the query `q` with exactly one letter wrong, extra,
// missing or swapped, anywhere. Digits are never edited (see one_edit_prefix).
bool one_edit_away(std::string_view name, std::string_view q) {
  const size_t n = name.size(), m = q.size();
  if (n + 1 < m || m + 1 < n) return false;
  size_t i = 0;
  while (i < n && i < m && fold(uint8_t(name[i])) == uint8_t(q[i])) ++i;
  if (i == n && i == m) return false;  // identical: not a typo
  if ((i < m && is_digit(uint8_t(q[i]))) || (i < n && is_digit(uint8_t(name[i])))) return false;
  auto same = [&](size_t a, size_t b) {  // name[a..] equals q[b..]
    if (n - a != m - b) return false;
    for (; a < n; ++a, ++b)
      if (fold(uint8_t(name[a])) != uint8_t(q[b])) return false;
    return true;
  };
  if (n == m) {
    if (same(i + 1, i + 1)) return true;  // wrong letter
    return i + 1 < n && fold(uint8_t(name[i])) == uint8_t(q[i + 1]) && fold(uint8_t(name[i + 1])) == uint8_t(q[i]) &&
           same(i + 2, i + 2);  // swapped
  }
  return n > m ? same(i + 1, i) : same(i, i + 1);  // missing / extra letter
}

// A whole-name match with one typo: scored as if typed right, minus the
// typo cost. This catches edits anywhere, where typo_score only fixes the
// start of a word.
int32_t whole_typo_score(std::string_view name, std::string_view q) {
  if (name.size() > 128 || !one_edit_away(name, q)) return kNoMatch;
  char fixed[128];
  for (size_t i = 0; i < name.size(); ++i) fixed[i] = char(fold(uint8_t(name[i])));
  const int32_t sc = fuzzy_capped(name, std::string_view(fixed, name.size()), 100);
  return sc == kNoMatch ? kNoMatch : sc - kTypoCost;
}

bool takes_typos(const Token& t) { return t.mode == Mode::Fuzzy && t.text.size() >= kTypoMinLen; }

bool is_subseq(std::string_view name, std::string_view q) { return subseq_end(name, q) != std::string_view::npos; }

size_t find_ci(std::string_view name, std::string_view q) {
  if (q.size() > name.size()) return std::string_view::npos;
  const size_t last = name.size() - q.size();
  for (size_t i = 0;; ++i) {
    i = find_folded(name, i, uint8_t(q[0]));
    if (i == std::string_view::npos || i > last) return std::string_view::npos;
    if (fold_eq(name.data() + i, q)) return i;
  }
}

bool token_matches(std::string_view name, const Token& t) {
  if (t.mode == Mode::Fuzzy) return is_subseq(name, t.text);
  return token_score(name, ~uint64_t(0), t) != kNoMatch;
}

bool ext_ok(std::string_view name, const std::vector<std::string>& exts) {
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos) return false;
  const std::string_view e = name.substr(dot + 1);
  for (const auto& x : exts)
    if (x.size() == e.size() && fold_eq(e.data(), x)) return true;
  return false;
}

// Bit i set if any token fits masks[i], for up to 64 masks. The compare
// loop is branch-free so it vectorizes; packing to bits is a second pass.
uint64_t fit_bits(const uint64_t* in, uint32_t n, const Token* const* toks, size_t nt) {
  alignas(64) uint64_t tail[64] = {};
  const uint64_t* masks = in;
  if (n < 64) {  // the section's last block: don't read past its masks
    std::memcpy(tail, in, n * sizeof(uint64_t));
    masks = tail;
  }
  alignas(64) uint8_t ok[64] = {};
  for (size_t t = 0; t < nt; ++t) {
    const uint64_t tm = toks[t]->mask, keep = ~toks[t]->loose, start = toks[t]->start;
    if (keep == ~uint64_t(0) && start == 0) {
      for (uint32_t i = 0; i < 64; ++i) ok[i] |= uint8_t((tm & ~masks[i]) == 0);
    } else {
      for (uint32_t i = 0; i < 64; ++i) {
        const uint64_t m = masks[i], miss = tm & ~m;
        const uint64_t typo = ((miss & keep) | (miss & (miss - 1))) | uint64_t((m & start) == 0);
        ok[i] |= uint8_t((miss == 0) | (typo == 0));
      }
    }
  }
  uint64_t bits = 0;
  for (uint32_t i = 0; i < 64; ++i) bits |= uint64_t(ok[i]) << i;
  return n == 64 ? bits : bits & ((uint64_t(1) << n) - 1);
}

// ---------------------------------------------------------------- ranking

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

uint8_t name_flags(std::string_view name) {
  uint8_t f = 0;
  if (name.starts_with('.')) f |= kNameDot;
  if (name.ends_with(".app")) f |= kNameApp;
  return f;
}

// Small per-entry nudges on top of match quality and the location prior.
int32_t rank_tweaks(uint8_t nflags, bool dir_or_link, uint8_t eflags, uint32_t mtime, uint32_t now) {
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
Key rank_key(int32_t score, uint32_t folder, uint32_t e) {
  const uint64_t hi = (uint64_t(int64_t(score) - int64_t(INT32_MIN)) << 32) | uint32_t(~folder);
  return (Key(hi) << 32) | uint32_t(~e);
}
uint32_t key_entry(Key k) { return ~uint32_t(k); }
int32_t key_score(Key k) { return int32_t(int64_t(uint64_t(k >> 64) & 0xffffffff) + INT32_MIN); }

// The most an entry adds to its name's score: the best folder prior plus
// every rank tweak.
constexpr int32_t kMaxEntryBonus = 60 + 25 + 10;

int32_t token_bound(const Token& t, std::string_view name) {
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
  std::vector<Key> buf;
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
    floor = *std::min_element(buf.begin(), buf.end());
  }
};

struct alignas(128) Chunk {
  uint64_t count = 0;
  TopK top{0};
  std::vector<uint32_t> all;  // each_match: every hit, unranked
};

// Jobs of roughly equal byte size over the blocks of a section.
template <typename Push>
void split_blocks(const Section& sec, size_t per_job, Push&& push) {
  for (uint32_t b = 0, nb = sec.blocks(); b < nb;) {
    uint32_t e = b + 1;
    while (e < nb && sec.block_off[e] - sec.block_off[b] < per_job) ++e;
    push(b, e);
    b = e;
  }
}

struct Job {
  int section;   // 0-3
  bool entries;  // [a, b) is an entry range (no name predicates), else blocks
  uint32_t a, b;
};

// Everything one query needs while visiting names and entries.
struct Scan {
  const Index& ix;
  const Query& q;
  std::vector<const Token*> pos, neg;
  bool need_dirs = false;
  bool names_matter = false;  // any name predicate: tokens, exts, re:
  bool dirs_ok = true, files_ok = true;
  uint32_t scope = UINT32_MAX;  // in: directory
  bool scope_missing = false;
  uint32_t now = 0;
  std::array<uint8_t, 256> file_ok{};  // per kind byte: 0 reject, 1 accept, 2 check exact size
  bool no_entry_filters = false;       // every entry of a matching name matches
  std::vector<DirHit>* dir_hits = nullptr;
  const std::vector<uint64_t>* dead = nullptr;

  Scan(const Index& ix_, const Query& q_) : ix(ix_), q(q_) {
    for (const Token& t : q.tokens) (t.negate ? neg : pos).push_back(&t);
    need_dirs = pos.size() > 1 || !neg.empty();
    names_matter = !q.tokens.empty() || !q.exts.empty() || q.name_re;
    now = uint32_t(std::time(nullptr));
    dirs_ok = (q.types & kTypeDir) && !q.has_size && q.kind != Kind::File && q.kind != Kind::Link;
    files_ok = (q.types & kTypeFiles) && q.kind != Kind::Dir;
    for (int k = 0; k < 256; ++k) {
      const int c = k & 15, cls = k >> 4;
      const uint64_t lo = class_lo(cls), hi = class_hi(cls);
      const uint8_t cls_ok = !q.has_size ? 1 : (hi < q.min_size || lo > q.max_size) ? 0
                             : (q.min_size <= lo && hi <= q.max_size)               ? 1
                                                                                    : 2;
      file_ok[size_t(k)] = ((q.types >> c) & 1) ? cls_ok : 0;
    }
    if (!q.scope.empty()) {
      scope = ix.lookup(q.scope, true);
      scope_missing = scope == UINT32_MAX || !ix.is_dir(scope);
    }
    no_entry_filters = dirs_ok && files_ok && q.kind == Kind::Any && q.types == kTypeAny && !q.has_size &&
                       q.min_mtime == 0 && q.max_mtime == UINT32_MAX && q.scope.empty() && !q.path_re;
  }

  // Name predicates for one distinct name; false if it can't matter.
  bool name_hit(std::string_view name, uint64_t m, NameHit& h) const {
    bool any = pos.empty();
    for (const Token* t : pos) any = any || t->fits(m);
    if (!any) {
      bool n = false;
      for (const Token* t : neg) n = n || t->fits(m);
      if (!n) return false;
    }
    h = NameHit{};
    h.flags = name_flags(name);
    int32_t total = 0;
    for (size_t t = 0; t < pos.size(); ++t) {
      if (!pos[t]->fits(m)) continue;
      const int32_t s = token_score(name, m, *pos[t]);
      if (s == kNoMatch) continue;
      h.bits |= uint8_t(1u << t);
      total += s;
      if (t < 4) h.best[t] = int16_t(std::clamp(s, 0, 32767));
    }
    h.score = int16_t(std::clamp(total, -32768, 32767));
    for (const Token* t : neg)
      if (t->fits(m) && token_matches(name, *t)) h.flags |= kNameNeg;
    const bool ok = (pos.empty() || h.bits) && !(h.flags & kNameNeg) && (q.exts.empty() || ext_ok(name, q.exts)) &&
                    (!q.name_re || q.name_re->matches(name));
    if (ok) h.flags |= kNameOk;
    return ok || h.bits || (h.flags & kNameNeg);
  }

  // Filters that don't depend on the name; true if entry `e` passes.
  bool entry_ok(uint32_t e) const {
    if (e == 0) return false;
    if (dead && ((*dead)[e >> 6] >> (e & 63)) & 1) return false;
    const bool dir = ix.is_dir(e);
    if (dir ? !dirs_ok : !files_ok) return false;
    const uint8_t fl = ix.flags[e];
    if (q.kind == Kind::Link && !(fl & kFlagLink)) return false;
    if (q.kind == Kind::File && (fl & kFlagLink)) return false;
    if (!dir) {
      const uint8_t v = file_ok[ix.file_kind[e - ix.dirs]];
      if (v == 0 || (v == 2 && (ix.size(e) < q.min_size || ix.size(e) > q.max_size))) return false;
    }
    const uint32_t mt = ix.mtime[e];
    if (mt < q.min_mtime || mt > q.max_mtime) return false;
    if (scope != UINT32_MAX && !ix.under(e, scope)) return false;
    return true;
  }

  // Score of entry `e` named by hit `h`, or kNoMatch. Tokens the name lacks
  // must be matched by a folder above it.
  int32_t entry_score(uint32_t e, const NameHit& h) const {
    int32_t score = h.score;
    const uint32_t p = ix.parent(e);
    if (need_dirs) {
      uint8_t bits = 0;
      std::array<int16_t, 4> best{};
      for (uint32_t a = p;; a = ix.parent(a)) {
        const DirHit& d = (*dir_hits)[a];
        if (d.neg) return kNoMatch;
        bits |= d.bits;
        for (size_t t = 0; t < 4; ++t) best[t] = std::max(best[t], d.best[t]);
        if (a == 0) break;
      }
      const uint8_t all = uint8_t((1u << pos.size()) - 1);
      if (uint8_t(h.bits | bits) != all) return kNoMatch;
      for (size_t t = 0; t < pos.size(); ++t)
        if (!(h.bits & (1u << t))) score += t < 4 ? best[t] * 3 / 4 : 6;
    }
    const bool dir_or_link = ix.is_dir(e) || (ix.flags[e] & kFlagLink);
    return score + ix.dir_prior[p] + rank_tweaks(h.flags, dir_or_link, ix.flags[e], ix.mtime[e], now);
  }

  bool path_ok(uint32_t e) const { return !q.path_re || q.path_re->matches(ix.path(e)); }

  // Visits names in blocks [a, b) of section `s`: for each name that can
  // matter, calls visit(first entry, count, hit).
  // Visits names in blocks [a, b) of section `s` whose mask a token fits:
  // visit(first entry, count, name, mask). Masks are tested 64 names at a
  // time without branches, so most blocks are skipped after a few vector ops.
  template <typename Visit>
  void candidates(int si, uint32_t a, uint32_t b, Visit&& visit) const {
    const Section& s = ix.sections[size_t(si)];
    char buf[256 + 32];
    std::array<const Token*, 16> toks{};
    size_t nt = 0;
    for (const Token* t : pos) toks[nt++] = t;
    for (const Token* t : neg)
      if (nt < toks.size()) toks[nt++] = t;
    const bool all = pos.empty();
    for (uint32_t blk = a; blk < b; ++blk) {
      const uint32_t u0 = blk * kBlock, un = std::min(kBlock, s.names() - u0);
      const uint64_t* masks = s.mask.p + u0;
      uint64_t hits = all ? (un == 64 ? ~uint64_t(0) : (uint64_t(1) << un) - 1) : 0;
      if (!all) hits = fit_bits(masks, un, toks.data(), nt);
      if (!hits) continue;
      const char* bs = s.bytes.p + s.block_off[blk];
      const char* be = s.bytes.p + s.block_off[blk + 1];
      uint32_t e = s.block_first[blk];
      uint32_t i = 0;
      while (hits) {
        const uint32_t next = uint32_t(__builtin_ctzll(hits));
        for (; i < next; ++i) e += s.count(u0 + i);
        const uint32_t k = u0 + i;
        const uint32_t c = s.count(k);
        std::string_view nm;
        if (s.hex) {
          size_t len;
          unpack_hex(bs + s.name_off[k], buf, len);
          nm = {buf, len};
        } else {
          const char* p = bs + s.name_off[k];
          const char* end = (i + 1 < un ? bs + s.name_off[k + 1] : be) - 1;
          nm = {p, size_t(end - p)};
        }
        visit(e, c, nm, masks[i]);
        e += c;
        ++i;
        hits &= hits - 1;
      }
    }
  }

  // Like candidates(), but with the name predicates applied: visit(first
  // entry, count, hit) for each name that can matter.
  template <typename Visit>
  void names(int si, uint32_t a, uint32_t b, Visit&& visit) const {
    candidates(si, a, b, [&](uint32_t e, uint32_t c, std::string_view nm, uint64_t m) {
      NameHit h;
      if (name_hit(nm, m, h)) visit(e, c, h);
    });
  }

  // The most a name could score: lets a search skip scoring names that
  // cannot reach its current top results (it still counts their matches).
  int32_t name_bound(std::string_view name) const {
    int32_t total = 0;
    for (const Token* t : pos) total += token_bound(*t, name);
    return total;
  }

  // Whether a name matches every name predicate (single-token queries).
  bool name_matches(std::string_view name, uint64_t m) const {
    const Token& t = *pos[0];
    if (!t.fits(m)) return false;
    bool hit;
    if (t.mode == Mode::Fuzzy) {
      hit = (t.mask & ~m) == 0 && is_subseq(name, t.text);
      if (!hit && takes_typos(t) && (m & t.start))
        hit = typo_score(name, m, t.text) != kNoMatch || whole_typo_score(name, t.text) != kNoMatch;
    } else {
      hit = token_score(name, m, t) != kNoMatch;
    }
    return hit && (q.exts.empty() || ext_ok(name, q.exts));
  }
};

}  // namespace

namespace {

// Score of an overlay entry, or kNoMatch: the same filters and scoring as
// an index entry, with its path's folders standing in for the folder hits
// and the nearest indexed folder's prior.
int32_t extra_score(const Scan& sc, const Extra& x, std::unordered_map<std::string, int8_t>& priors) {
  const Query& q = sc.q;
  const size_t cut = x.path.rfind('/');
  const std::string_view name = std::string_view(x.path).substr(cut + 1);
  const std::string_view dir = std::string_view(x.path).substr(0, cut == 0 ? 1 : cut);
  if (name.empty()) return kNoMatch;
  if (x.dir ? !sc.dirs_ok : !sc.files_ok) return kNoMatch;
  if (q.kind == Kind::Link && !(x.flags & kFlagLink)) return kNoMatch;
  if (q.kind == Kind::File && (x.flags & kFlagLink)) return kNoMatch;
  if (!x.dir) {
    const uint8_t v = sc.file_ok[x.kind];
    if (v == 0 || (v == 2 && (x.size < q.min_size || x.size > q.max_size))) return kNoMatch;
  }
  if (x.mtime < q.min_mtime || x.mtime > q.max_mtime) return kNoMatch;
  if (!q.scope.empty() && !(std::string_view(x.path).starts_with(q.scope) && x.path.size() > q.scope.size() &&
                            (q.scope == "/" || x.path[q.scope.size()] == '/')))
    return kNoMatch;
  NameHit h;
  if (!sc.name_hit(name, x.mask, h) || !(h.flags & kNameOk)) return kNoMatch;
  int32_t score = h.score;
  if (sc.need_dirs) {
    uint8_t bits = 0;
    std::array<int32_t, 8> best{};
    for (size_t i = 0; i < dir.size();) {
      size_t j = dir.find('/', i);
      if (j == std::string_view::npos) j = dir.size();
      const std::string_view comp = dir.substr(i, j - i);
      i = j + 1;
      if (comp.empty()) continue;
      for (const Token* t : sc.neg)
        if (token_matches(comp, *t)) return kNoMatch;
      for (size_t t = 0; t < sc.pos.size(); ++t) {
        const int32_t sv = token_score(comp, ~uint64_t(0), *sc.pos[t]);
        if (sv == kNoMatch) continue;
        bits |= uint8_t(1u << t);
        best[t] = std::max(best[t], std::clamp(sv, 0, 32767));
      }
    }
    const uint8_t all = uint8_t((1u << sc.pos.size()) - 1);
    if (uint8_t(h.bits | bits) != all) return kNoMatch;
    for (size_t t = 0; t < sc.pos.size(); ++t)
      if (!(h.bits & (1u << t))) score += t < 4 ? best[t] * 3 / 4 : 6;
  }
  if (q.path_re && !q.path_re->matches(x.path)) return kNoMatch;
  // The prior of the nearest folder the index has.
  auto it = priors.find(std::string(dir));
  if (it == priors.end()) {
    int8_t prior = 0;
    for (std::string_view up = dir; !up.empty();) {
      const uint32_t d = sc.ix.lookup(up);
      if (d != UINT32_MAX && sc.ix.is_dir(d)) {
        prior = sc.ix.dir_prior[d];
        break;
      }
      const size_t slash = up.rfind('/');
      if (slash == std::string_view::npos || up == "/") break;
      up = slash == 0 ? std::string_view("/") : up.substr(0, slash);
    }
    it = priors.emplace(std::string(dir), prior).first;
  }
  return score + it->second + rank_tweaks(h.flags, x.dir || (x.flags & kFlagLink), x.flags, x.mtime, sc.now);
}

}  // namespace

// ---------------------------------------------------------------- scoring API

bool path_matches(const Query& q, std::string_view path) {
  const size_t cut = path.rfind('/');
  const std::string_view name = cut == std::string_view::npos ? path : path.substr(cut + 1);
  const std::string_view dirs = cut == std::string_view::npos ? std::string_view() : path.substr(0, cut);
  if (!q.exts.empty() && !ext_ok(name, q.exts)) return false;
  if ((q.types & kTypeFiles) != kTypeFiles && !((q.types >> categorize(name)) & 1)) return false;
  if (q.name_re && !q.name_re->matches(name)) return false;
  if (q.path_re && !q.path_re->matches(path)) return false;
  const uint64_t m = name_mask(name);
  auto any_comp = [&](auto&& fn) {
    for (size_t i = 0; i < dirs.size();) {
      size_t j = dirs.find('/', i);
      if (j == std::string_view::npos) j = dirs.size();
      if (j > i && fn(dirs.substr(i, j - i))) return true;
      i = j + 1;
    }
    return false;
  };
  for (const Token& t : q.tokens) {
    if (t.negate) {
      if (token_matches(name, t) || any_comp([&](std::string_view c) { return token_matches(c, t); })) return false;
    } else if (!(t.fits(m) && token_score(name, m, t) != kNoMatch) &&
               !any_comp([&](std::string_view c) { return token_score(c, ~uint64_t(0), t) != kNoMatch; })) {
      return false;
    }
  }
  return true;
}

int32_t fuzzy_score(std::string_view name, std::string_view q) { return fuzzy_capped(name, q, 100); }

int32_t token_score(std::string_view name, uint64_t m, const Token& t) {
  const int32_t len = length_cost(name);
  switch (t.mode) {
    case Mode::Fuzzy:
      if (takes_typos(t)) {
        const int32_t clean = (t.mask & ~m) == 0 ? fuzzy_score(name, t.text) : kNoMatch;
        const int32_t typo = (m & t.start) ? std::max(typo_score(name, m, t.text), whole_typo_score(name, t.text))
                                           : kNoMatch;
        return std::max(clean, typo);
      }
      return fuzzy_score(name, t.text);
    case Mode::Exact: {
      const size_t p = find_ci(name, t.text);
      return p == std::string_view::npos ? kNoMatch : 40 + (p == 0 ? 30 : 0) - len;
    }
    case Mode::Prefix:
      return starts_fold(name, t.text) ? 60 - len : kNoMatch;
    case Mode::Suffix:
      return name.size() >= t.text.size() && fold_eq(name.data() + name.size() - t.text.size(), t.text) ? 50 - len
                                                                                                         : kNoMatch;
  }
  return kNoMatch;
}

// ---------------------------------------------------------------- parsing

namespace {

uint64_t parse_size(std::string_view s, bool& ok) {
  size_t i = 0;
  double v = 0;
  bool digits = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, digits = true) v = v * 10 + (s[i] - '0');
  if (i < s.size() && s[i] == '.') {
    double scale = 0.1;
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, scale /= 10, digits = true) v += (s[i] - '0') * scale;
  }
  std::string unit;
  for (; i < s.size(); ++i) unit += char(kFold[uint8_t(s[i])]);
  double mult = 0;
  if (unit.empty() || unit == "b") mult = 1;
  else if (unit == "k" || unit == "kb" || unit == "kib") mult = 1024.0;
  else if (unit == "m" || unit == "mb" || unit == "mib") mult = 1024.0 * 1024;
  else if (unit == "g" || unit == "gb" || unit == "gib") mult = 1024.0 * 1024 * 1024;
  else if (unit == "t" || unit == "tb" || unit == "tib") mult = 1024.0 * 1024 * 1024 * 1024;
  ok = digits && mult > 0;
  return uint64_t(v * mult);
}

// An age like 7d, 3h, 2w, 6mo, 1y (bare numbers are days), in seconds.
uint64_t parse_age(std::string_view s, bool& ok) {
  size_t i = 0;
  double v = 0;
  bool digits = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, digits = true) v = v * 10 + (s[i] - '0');
  if (i < s.size() && s[i] == '.') {
    double scale = 0.1;
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, scale /= 10, digits = true) v += (s[i] - '0') * scale;
  }
  const std::string_view u = s.substr(i);
  double mult = 0;
  if (u == "s") mult = 1;
  else if (u == "m" || u == "min") mult = 60;
  else if (u == "h") mult = 3600;
  else if (u.empty() || u == "d") mult = 86400;
  else if (u == "w") mult = 604800;
  else if (u == "mo") mult = 2592000;
  else if (u == "y") mult = 31536000;
  ok = digits && mult > 0;
  return uint64_t(v * mult);
}

// Kinds one type: value allows, or 0 if unknown.
uint32_t type_bits(std::string_view v) {
  if (v == "dir" || v == "folder" || v == "directory") return kTypeDir;
  if (v == "file") return kTypeFiles;
  if (v == "video" || v == "movie") return 1u << CatVideo;
  if (v == "audio" || v == "music") return 1u << CatAudio;
  if (v == "image" || v == "photo" || v == "picture") return 1u << CatImage;
  if (v == "doc" || v == "document") return 1u << CatDoc;
  if (v == "archive" || v == "zip") return 1u << CatArchive;
  if (v == "code" || v == "source") return 1u << CatCode;
  if (v == "font") return 1u << CatFont;
  return 0;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  for (size_t i = 0; i <= s.size();) {
    size_t j = s.find(sep, i);
    if (j == std::string_view::npos) j = s.size();
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

std::string fold_str(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(kFold[uint8_t(c)]);
  return out;
}

// Words split on spaces; "double quotes" keep spaces inside one word.
std::vector<std::string> split_words(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : s) {
    if (c == '"') {
      quoted = !quoted;
    } else if (c == ' ' && !quoted) {
      if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

void push_token(Query& q, std::string_view w) {
  Token t;
  if (w.starts_with('!')) w.remove_prefix(1), t.negate = true, t.mode = Mode::Exact;
  if (w.starts_with('\'')) w.remove_prefix(1), t.mode = Mode::Exact;
  else if (w.starts_with('^')) w.remove_prefix(1), t.mode = Mode::Prefix;
  else if (w.ends_with('$')) w.remove_suffix(1), t.mode = Mode::Suffix;
  if (w.empty()) return;
  size_t positive = 0;
  for (const Token& x : q.tokens) positive += !x.negate;
  if (!t.negate && positive >= 8) return;  // matched tokens are tracked in 8 bits
  t.text = fold_str(w);
  for (char c : t.text) t.mask |= char_bit(uint8_t(c));
  if (takes_typos(t)) {
    t.loose = t.mask & ~char_bit(uint8_t(t.text[0]));
    t.start = start_bit(uint8_t(t.text[0]));
  }
  q.tokens.push_back(std::move(t));
}

// "<x", "<=x", ">x", ">=x", "a..b", "=x" or bare "x" as [lo, hi].
template <typename Parse>
bool parse_range(std::string_view v, Parse&& p, uint64_t& lo, uint64_t& hi, bool bare_is_max) {
  bool ok = false;
  lo = 0, hi = UINT64_MAX;
  if (v.starts_with(">=")) lo = p(v.substr(2), ok);
  else if (v.starts_with('>')) lo = p(v.substr(1), ok) + 1;
  else if (v.starts_with("<=")) hi = p(v.substr(2), ok);
  else if (v.starts_with('<')) {
    const uint64_t x = p(v.substr(1), ok);
    if (x == 0) lo = UINT64_MAX, hi = 0;
    else hi = x - 1;
  } else if (v.starts_with('=')) lo = hi = p(v.substr(1), ok);
  else if (const size_t dots = v.find(".."); dots != std::string_view::npos) {
    bool ok2 = false;
    lo = p(v.substr(0, dots), ok);
    hi = p(v.substr(dots + 2), ok2);
    ok = ok && ok2;
  } else if (bare_is_max) hi = p(v, ok);
  else lo = p(v, ok);
  return ok;
}

}  // namespace

bool Query::has_tokens() const { return !tokens.empty(); }

bool Query::selects() const {
  return !tokens.empty() || !exts.empty() || types != kTypeAny || kind != Kind::Any || !scope.empty() || has_size ||
         min_mtime != 0 || max_mtime != UINT32_MAX || name_re || path_re;
}

Query parse_query(std::string_view text, std::string_view home) {
  Query q;
  auto bad = [&](std::string_view tok, std::string why = "ignored ") {
    if (q.error.empty()) q.error = why + std::string(tok);
  };
  for (const std::string& word : split_words(text)) {
    const size_t colon = word.find(':');
    if (colon != std::string::npos && colon > 0) {
      const std::string key = fold_str(std::string_view(word).substr(0, colon));
      const std::string_view raw = std::string_view(word).substr(colon + 1);
      const std::string low = fold_str(raw);
      const std::string_view v = low;
      bool known = true;
      if (key == "size") {
        uint64_t lo, hi;
        if (!parse_range(v, parse_size, lo, hi, false)) bad(word);
        else {
          q.has_size = true;
          q.min_size = std::max(q.min_size, lo);
          q.max_size = std::min(q.max_size, hi);
        }
      } else if (key == "mtime" || key == "modified") {
        // mtime:<7d is "modified within the last 7 days"; mtime:>1y "longer ago".
        uint64_t lo, hi;
        if (!parse_range(v, parse_age, lo, hi, true)) bad(word);
        else {
          const uint64_t now = uint64_t(std::time(nullptr));
          q.min_mtime = std::max<uint32_t>(q.min_mtime, hi >= now ? 0 : uint32_t(now - hi));
          q.max_mtime = std::min<uint32_t>(q.max_mtime, lo >= now ? 0 : uint32_t(now - lo));
        }
      } else if (key == "type") {
        uint32_t any = 0;
        for (auto p : split(v, ',')) {
          if (p == "app") {
            q.kind = Kind::Dir;
            q.exts.push_back("app");
            any |= kTypeDir;
            continue;
          }
          const uint32_t bits = type_bits(p);
          if (!bits) bad(word, "unknown type in ");
          any |= bits;
        }
        if (any) q.types &= any;
      } else if (key == "kind") {
        if (v == "file" || v == "f") q.kind = Kind::File;
        else if (v == "dir" || v == "folder" || v == "d") q.kind = Kind::Dir;
        else if (v == "link" || v == "symlink" || v == "l") q.kind = Kind::Link;
        else bad(word, "unknown kind in ");
      } else if (key == "ext") {
        for (auto p : split(v, ',')) q.exts.push_back(std::string(p.starts_with('.') ? p.substr(1) : p));
      } else if (key == "in") {
        std::string p(raw);
        if (p.starts_with('~')) p = std::string(home) + p.substr(1);
        if (char* real = realpath(p.c_str(), nullptr)) {  // the index holds real paths: /etc is /private/etc
          p = real;
          std::free(real);
        }
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        q.scope = p;
      } else if (key == "re" || key == "path") {
        std::string err;
        auto re = Regex::compile(raw, true, false, err);
        if (!re) bad(word, err + " in ");
        (key == "re" ? q.name_re : q.path_re) = re;
      } else if (key == "limit") {
        q.limit = size_t(std::strtoull(low.c_str(), nullptr, 10));
      } else if (key == "sym" || key == "symbol") {
        if (!v.empty()) q.syms.push_back(std::string(v));
      } else if (key == "grep" || key == "content") {
        q.grep = raw, q.grep_mode = GrepMode::Literal;
      } else if (key == "regex") {
        q.grep = raw, q.grep_mode = GrepMode::Regex;
      } else {
        known = false;
      }
      if (known) continue;
    }
    for (auto piece : split(word, '/')) push_token(q, piece);
  }
  return q;
}

// ---------------------------------------------------------------- search

namespace {

std::vector<Job> make_jobs(const Index& ix, const Scan& sc, size_t target) {
  std::vector<Job> jobs;
  if (!sc.names_matter && sc.neg.empty()) {
    // Filters only: walk entries instead of names.
    for (int s = 0; s < 4; ++s) {
      if (s < 2 ? !sc.dirs_ok : !sc.files_ok) continue;
      const Section& sec = ix.sections[size_t(s)];
      const uint32_t first = sec.first_entry(), count = sec.end_entry() - first;
      const size_t parts = std::max<size_t>(1, target * count / std::max<uint32_t>(1, ix.n));
      for (size_t j = 0; j < parts; ++j) {
        const uint32_t a = first + uint32_t(count * j / parts), b = first + uint32_t(count * (j + 1) / parts);
        if (a < b) jobs.push_back({s, true, a, b});
      }
    }
    return jobs;
  }
  size_t bytes = 0;
  for (int s = 0; s < 4; ++s)
    if (s < 2 ? sc.dirs_ok : sc.files_ok) bytes += ix.sections[size_t(s)].block_off[ix.sections[size_t(s)].blocks()];
  const size_t per_job = std::max<size_t>(1, bytes / target);
  for (int s = 0; s < 4; ++s)
    if (s < 2 ? sc.dirs_ok : sc.files_ok)
      split_blocks(ix.sections[size_t(s)], per_job, [&](uint32_t b, uint32_t e) { jobs.push_back({s, false, b, e}); });
  return jobs;
}

// Fills `hits` with every directory's own token matches, for entries whose
// tokens are matched by a folder above them.
void fill_dir_hits(const Index& ix, Scan& sc, Pool& pool, std::vector<DirHit>& hits) {
  hits.assign(ix.dirs, DirHit{});
  std::vector<std::pair<int, std::pair<uint32_t, uint32_t>>> jobs;
  const size_t per = std::max<size_t>(1, (ix.sections[0].block_off[ix.sections[0].blocks()] +
                                          ix.sections[1].block_off[ix.sections[1].blocks()]) /
                                             (size_t(pool.size()) * 8));
  for (int s = 0; s < 2; ++s)
    split_blocks(ix.sections[size_t(s)], per, [&](uint32_t b, uint32_t e) { jobs.push_back({s, {b, e}}); });
  pool.run(jobs.size(), [&](size_t j) {
    sc.names(jobs[j].first, jobs[j].second.first, jobs[j].second.second,
             [&](uint32_t e0, uint32_t c, const NameHit& h) {
               DirHit d;
               d.bits = h.bits;
               d.neg = h.flags & kNameNeg;
               d.best = h.best;
               for (uint32_t e = e0; e < e0 + c; ++e) hits[e] = d;
             });
  });
}

}  // namespace

void Engine::each_match(const Index& ix, const Query& q, bool files_only, const std::function<void(uint32_t)>& take) {
  if (ix.count() == 0) return;
  Scan sc(ix, q);
  if (sc.scope_missing) return;
  if (files_only) sc.dirs_ok = false;
  std::vector<DirHit> dir_hits;
  if (sc.need_dirs) {
    fill_dir_hits(ix, sc, pool_, dir_hits);
    sc.dir_hits = &dir_hits;
  }
  const std::vector<Job> jobs = make_jobs(ix, sc, size_t(pool_.size()) * 24);
  NameHit any;
  any.flags = kNameOk;
  pool_.run(jobs.size(), [&](size_t j) {
    const Job& job = jobs[j];
    auto entry = [&](uint32_t e, const NameHit& h) {
      if (sc.entry_ok(e) && sc.entry_score(e, h) != kNoMatch && sc.path_ok(e)) take(e);
    };
    if (job.entries) {
      for (uint32_t e = job.a; e < job.b; ++e) entry(e, any);
      return;
    }
    sc.names(job.section, job.a, job.b, [&](uint32_t e0, uint32_t c, const NameHit& h) {
      if (!(h.flags & kNameOk)) return;
      for (uint32_t e = e0; e < e0 + c; ++e) entry(e, h);
    });
  });
}

Results Engine::search(const Index& ix, const Query& q, size_t limit, const Overlay* overlay) {
  Results r;
  if (ix.count() == 0 || !q.selects()) return r;
  Scan sc(ix, q);
  if (overlay) sc.dead = overlay->dead;
  // New folders aren't in the index yet; their entries are in the overlay.
  const bool scope_new = sc.scope_missing && overlay && !overlay->extras.empty();
  if (sc.scope_missing && !scope_new) return r;
  std::vector<DirHit> dir_hits;
  if (sc.need_dirs) {
    fill_dir_hits(ix, sc, pool_, dir_hits);
    sc.dir_hits = &dir_hits;
  }
  const std::vector<Job> jobs = scope_new ? std::vector<Job>() : make_jobs(ix, sc, size_t(pool_.size()) * 24);
  std::vector<Chunk> out(jobs.size());
  for (auto& c : out) c.top = TopK(limit);
  // Names that can't beat the k-th best result so far are only counted. The
  // floor is shared: every job's k-th best bounds the overall k-th best.
  const bool prune = limit > 0 && sc.pos.size() == 1 && !sc.need_dirs && !q.name_re && !q.path_re;
  std::atomic<int32_t> shared_floor{INT32_MIN};
  NameHit any;
  any.flags = kNameOk;
  pool_.run(jobs.size(), [&](size_t j) {
    const Job& job = jobs[j];
    Chunk& o = out[j];
    auto entry = [&](uint32_t e, const NameHit& h) {
      if (!sc.entry_ok(e)) return;
      const int32_t s = sc.entry_score(e, h);
      if (s == kNoMatch || !sc.path_ok(e)) return;
      ++o.count;
      o.top.push(rank_key(s, ix.dir_pre[ix.parent(e)], e));
    };
    auto publish_floor = [&] {
      if (!o.top.full()) return;
      const int32_t f = key_score(o.top.floor);
      for (int32_t cur = shared_floor.load(std::memory_order_relaxed);
           f > cur && !shared_floor.compare_exchange_weak(cur, f, std::memory_order_relaxed);) {
      }
    };
    if (job.entries) {
      for (uint32_t e = job.a; e < job.b; ++e) entry(e, any);
    } else {
      size_t since = 0;
      sc.candidates(job.section, job.a, job.b, [&](uint32_t e0, uint32_t c, std::string_view nm, uint64_t m) {
        if (prune && sc.name_bound(nm) + kMaxEntryBonus < shared_floor.load(std::memory_order_relaxed)) {
          if (!sc.name_matches(nm, m)) return;
          if (sc.no_entry_filters) o.count += c;
          else
            for (uint32_t e = e0; e < e0 + c; ++e) o.count += sc.entry_ok(e);
          return;
        }
        NameHit h;
        if (!sc.name_hit(nm, m, h) || !(h.flags & kNameOk)) return;
        for (uint32_t e = e0; e < e0 + c; ++e) entry(e, h);
        if (prune && (since += c) >= 256) since = 0, publish_floor();
      });
    }
    o.top.cut();
    publish_floor();
  });
  std::vector<Key> keys;
  for (auto& o : out) {
    r.total += o.count;
    keys.insert(keys.end(), o.top.buf.begin(), o.top.buf.end());
  }
  const size_t k = std::min(limit, keys.size());
  std::partial_sort(keys.begin(), keys.begin() + long(k), keys.end(), std::greater<>());
  // Overlay entries are few: score them all and merge by score.
  std::vector<std::pair<int32_t, const Extra*>> extras;
  if (overlay) {
    std::unordered_map<std::string, int8_t> priors;
    for (const Extra* x : overlay->extras) {
      const int32_t s = extra_score(sc, *x, priors);
      if (s == kNoMatch) continue;
      ++r.total;
      extras.push_back({s, x});
    }
    std::stable_sort(extras.begin(), extras.end(), [](const auto& a, const auto& b) {
      return a.first != b.first ? a.first > b.first : a.second->path < b.second->path;
    });
  }
  size_t i = 0, j = 0;
  while (r.top.size() < limit && (i < k || j < extras.size())) {
    if (j >= extras.size() || (i < k && key_score(keys[i]) >= extras[j].first)) {
      r.top.push_back(key_entry(keys[i]));
      r.score.push_back(key_score(keys[i]));
      r.extra.push_back(nullptr);
      ++i;
    } else {
      r.top.push_back(UINT32_MAX);
      r.score.push_back(extras[j].first);
      r.extra.push_back(extras[j].second);
      ++j;
    }
  }
  return r;
}

Results Engine::search_symbols(const Index& ix, const SymbolIndex& sx, const Query& q, size_t limit) {
  Results r;
  if (q.syms.empty()) return r;
  Query fq = q;
  fq.syms.clear();
  const bool by_file = fq.selects();

  // Symbol names must contain every sym: term; the rest of the query picks
  // the files that define them.
  std::string_view needle;
  int skip = -1;
  for (int i = 0; i < int(q.syms.size()); ++i)
    if (q.syms[i].size() > needle.size()) needle = q.syms[i], skip = i;
  auto sym_ok = [&](std::string_view name) {
    for (int i = 0; i < int(q.syms.size()); ++i)
      if (i != skip && !contains_fold(name, q.syms[i])) return false;
    return true;
  };
  std::vector<uint64_t> files;
  if (by_file) {
    files.assign((ix.count() + 63) / 64, 0);
    each_match(ix, fq, true, [&](uint32_t e) {
      __atomic_fetch_or(&files[e / 64], uint64_t(1) << (e % 64), __ATOMIC_RELAXED);
    });
  }

  const Section& s = sx.names;
  std::vector<std::pair<uint32_t, uint32_t>> jobs;
  const size_t per_job = std::max<size_t>(1, s.block_off[s.blocks()] / (size_t(pool_.size()) * 24));
  split_blocks(s, per_job, [&](uint32_t b, uint32_t e) { jobs.emplace_back(b, e); });
  struct alignas(128) Out {
    uint64_t count = 0;
    std::vector<uint32_t> hits;
  };
  std::vector<Out> out(jobs.size());
  pool_.run(jobs.size(), [&](size_t j) {
    Out& o = out[j];
    auto take = [&](uint32_t o0, uint32_t o1) {
      for (uint32_t i = o0; i < o1; ++i) {
        const uint32_t e = sx.occ[i];
        if (by_file && !((files[e / 64] >> (e % 64)) & 1)) continue;
        ++o.count;
        if (o.hits.size() < limit) o.hits.push_back(i);
      }
    };
    for (uint32_t b = jobs[j].first; b < jobs[j].second; ++b) {
      const uint32_t u0 = b * kBlock, un = std::min(kBlock, s.names() - u0);
      const char* bs = s.bytes.p + s.block_off[b];
      const char* be = s.bytes.p + s.block_off[b + 1];
      const uint16_t* off = s.name_off.p + u0;
      auto name_at = [&](uint32_t i) {
        const char* p = bs + off[i];
        const char* end = (i + 1 < un ? bs + off[i + 1] : be) - 1;
        return std::string_view(p, size_t(end - p));
      };
      uint32_t e = s.block_first[b], i = 0;
      const char* p = bs;
      while ((p = find_fold(p, be, needle)) != be) {
        const uint32_t h = uint32_t(p - bs);
        while (i + 1 < un && off[i + 1] <= h) e += s.count(u0 + i++);
        const uint32_t c = s.count(u0 + i);
        if (sym_ok(name_at(i))) take(e, e + c);
        if (++i >= un) break;
        e += c;
        p = bs + off[i];
      }
    }
  });
  for (auto& o : out) {
    r.total += o.count;
    for (uint32_t i : o.hits) {
      if (r.top.size() >= limit) break;
      r.top.push_back(i);
    }
  }
  return r;
}

}  // namespace fplussearch
