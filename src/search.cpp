#include "search.hpp"

#include <algorithm>
#include <array>
#include <cstring>

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

namespace fplussearch {

namespace {

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

bool ends_with_fold(std::string_view hay, std::string_view suffix) {
  return hay.size() > suffix.size() && fold_eq(hay.data() + hay.size() - suffix.size(), suffix);
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
    uint8x16_t e0 = vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a), FM), F),
                             vceqq_u8(vorrq_u8(vld1q_u8(b), LM), L));
    uint8x16_t e1 = vandq_u8(vceqq_u8(vorrq_u8(vld1q_u8(a + 16), FM), F),
                             vceqq_u8(vorrq_u8(vld1q_u8(b + 16), LM), L));
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
  for (; p < limit; ++p) {
    if ((uint8_t(p[0]) | fm) == first && (uint8_t(p[k - 1]) | lm) == last && fold_eq(p, needle))
      return p;
  }
  return end;
#endif
}

// Name-only predicates (terms and extensions), evaluated once per distinct name.
// A hex needle as packed nibbles starting at an even (align 0) or odd
// (align 1) nibble: byte m of a match must satisfy (byte & mask[m]) == pat[m].
struct PackedNeedle {
  uint32_t align = 0, len = 0;  // len in bytes
  uint32_t lo = 0, hi = 0;      // bytes the SIMD filter tests, preferring full bytes
  std::array<uint8_t, 160> pat{}, mask{};
};

bool packed_at(const uint8_t* c, const PackedNeedle& n) {
  for (size_t m = 0; m < n.len; ++m)
    if ((c[m] & n.mask[m]) != n.pat[m]) return false;
  return true;
}

// Calls hit(position, needle) for every match of either alignment in
// [p, end), in increasing position order per 32-byte step.
template <typename Hit>
void scan_packed(const uint8_t* p, const uint8_t* end, const std::array<PackedNeedle, 2>& ns, Hit&& hit) {
  auto check = [&](const uint8_t* c, const PackedNeedle& n) {
    if (n.len && c + n.len <= end && packed_at(c, n)) hit(c, n);
  };
#if defined(__ARM_NEON)
  // A disabled alignment compares (x & 0) == 1, which never matches.
  auto lane = [](const PackedNeedle& n, uint32_t i, bool mask) {
    return vdupq_n_u8(n.len ? (mask ? n.mask[i] : n.pat[i]) : uint8_t(!mask));
  };
  const uint8x16_t AP0 = lane(ns[0], ns[0].lo, false), AM0 = lane(ns[0], ns[0].lo, true);
  const uint8x16_t AP1 = lane(ns[0], ns[0].hi, false), AM1 = lane(ns[0], ns[0].hi, true);
  const uint8x16_t BP0 = lane(ns[1], ns[1].lo, false), BM0 = lane(ns[1], ns[1].lo, true);
  const uint8x16_t BP1 = lane(ns[1], ns[1].hi, false), BM1 = lane(ns[1], ns[1].hi, true);
  auto eq = [](const uint8_t* q, uint8x16_t m, uint8x16_t pat) { return vceqq_u8(vandq_u8(vld1q_u8(q), m), pat); };
  for (; p < end; p += 32) {
    uint8x16_t r[4];
    for (int h = 0; h < 2; ++h) {
      const uint8_t* q = p + 16 * h;
      r[h] = vandq_u8(eq(q + ns[0].lo, AM0, AP0), eq(q + ns[0].hi, AM1, AP1));
      r[2 + h] = vandq_u8(eq(q + ns[1].lo, BM0, BP0), eq(q + ns[1].hi, BM1, BP1));
    }
    if (vmaxvq_u8(vorrq_u8(vorrq_u8(r[0], r[1]), vorrq_u8(r[2], r[3]))) == 0) continue;
    for (int j = 0; j < 4; ++j) {
      uint64_t mask = vget_lane_u64(vreinterpret_u64_u8(vshrn_n_u16(vreinterpretq_u16_u8(r[j]), 4)), 0);
      while (mask) {
        const int bit = __builtin_ctzll(mask) >> 2;
        check(p + 16 * (j % 2) + bit, ns[size_t(j / 2)]);
        mask &= ~(0xfull << (bit * 4));
      }
    }
  }
#else
  for (; p < end; ++p) {
    check(p, ns[0]);
    check(p, ns[1]);
  }
#endif
}

// Calls take(first, end) with the item range of every name in text block `b`
// that contains `needle` (every name if empty) and passes ok(name).
template <typename Ok, typename Take>
void scan_text_block(const Section& s, uint32_t b, std::string_view needle, Ok&& ok, Take&& take) {
  const uint32_t u0 = b * kBlock;
  const uint32_t un = std::min(kBlock, s.names() - u0);
  const char* bs = s.bytes.p + s.block_off[b];
  const char* be = s.bytes.p + s.block_off[b + 1];
  const uint16_t* off = s.name_off.p + u0;
  auto name_at = [&](uint32_t i) {
    const char* p = bs + off[i];
    const char* end = (i + 1 < un ? bs + off[i + 1] : be) - 1;
    return std::string_view(p, size_t(end - p));
  };
  uint32_t e = s.block_first[b];
  if (needle.empty()) {
    for (uint32_t i = 0; i < un; ++i) {
      const uint32_t c = s.count(u0 + i);
      if (ok(name_at(i))) take(e, e + c);
      e += c;
    }
    return;
  }
  uint32_t i = 0;
  const char* p = bs;
  while ((p = find_fold(p, be, needle)) != be) {
    const uint32_t h = uint32_t(p - bs);
    while (i + 1 < un && off[i + 1] <= h) e += s.count(u0 + i++);
    const uint32_t c = s.count(u0 + i);
    if (ok(name_at(i))) take(e, e + c);
    if (++i >= un) break;
    e += c;
    p = bs + off[i];
  }
}

// Jobs of roughly equal byte size over the given blocks of a section.
template <typename Push>
void split_blocks(const Section& sec, size_t per_job, Push&& push) {
  for (uint32_t b = 0, nb = sec.blocks(); b < nb;) {
    uint32_t e = b + 1;
    while (e < nb && sec.block_off[e] - sec.block_off[b] < per_job) ++e;
    push(b, e);
    b = e;
  }
}

struct alignas(128) Chunk {
  uint64_t count = 0;
  std::vector<uint32_t> hits;
};

bool all_hex(std::string_view s) {
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

// Per-query state shared by all jobs.
struct Ctx {
  const Index& ix;
  const Query& q;
  size_t limit;
  std::string_view needle;  // scanned for in name bytes; empty = test every name
  int skip;                 // term already satisfied by the needle
  bool file_filter;         // files need kind/size checks
  std::array<uint8_t, 256> file_ok;  // per kind byte: 0 reject, 1 accept, 2 check exact size
  std::array<uint8_t, 16> cat_ok;    // per Category: 0 or 0xff
  std::array<uint8_t, 16> class_ok;  // per size class: 0, 1 or 2
  std::array<PackedNeedle, 2> packed;  // needle at both nibble alignments, for hex sections

  void init_file_ok() {
    for (int c = 0; c < 16; ++c) {
      cat_ok[size_t(c)] = ((q.types >> c) & 1) ? 0xff : 0;
      const uint64_t lo = class_lo(c), hi = class_hi(c);
      class_ok[size_t(c)] = !q.has_size ? 1
                            : (hi < q.min_size || lo > q.max_size)   ? 0
                            : (q.min_size <= lo && hi <= q.max_size) ? 1
                                                                     : 2;
    }
    for (int k = 0; k < 256; ++k) file_ok[size_t(k)] = cat_ok[size_t(k & 15)] & class_ok[size_t(k >> 4)];
  }

  // Picks the needle (longest term) for name scans and prepares its packed
  // form; returns whether hex sections can match.
  bool init_names() {
    // Scan name bytes for the longest term; everything else is checked per name.
    for (int i = 0; i < int(q.terms.size()); ++i)
      if (q.terms[i].size() > needle.size()) needle = q.terms[i], skip = i;
    if (needle.empty() && q.exts.size() == 1) needle = q.exts[0];
    // Hex sections hold only [0-9a-f], so most queries skip them entirely.
    bool hex_ok = q.exts.empty();
    for (const auto& t : q.terms) hex_ok = hex_ok && all_hex(t);

    if (hex_ok && !needle.empty()) {
      for (uint32_t a = 0; a < 2; ++a) {
        PackedNeedle& pn = packed[a];
        pn.align = a;
        pn.len = uint32_t((a + needle.size() + 1) / 2);
        if (pn.len > pn.pat.size()) {
          pn.len = 0;  // longer than any hex name
          continue;
        }
        for (uint32_t j = 0; j < needle.size(); ++j) {
          const char c = needle[j];
          const uint8_t v = uint8_t(c <= '9' ? c - '0' : c - 'a' + 10);
          const uint32_t pos = a + j;
          pn.pat[pos / 2] |= pos % 2 ? v : uint8_t(v << 4);
          pn.mask[pos / 2] |= pos % 2 ? 0x0f : 0xf0;
        }
        pn.lo = 0;
        pn.hi = pn.len - 1;
        while (pn.lo < pn.hi && pn.mask[pn.lo] != 0xff) ++pn.lo;
        while (pn.hi > pn.lo && pn.mask[pn.hi] != 0xff) --pn.hi;
        if (pn.mask[pn.lo] != 0xff) pn.lo = 0, pn.hi = pn.len - 1;
        if (pn.lo == pn.hi && pn.len > 1) pn.hi = pn.lo + 1 < pn.len ? pn.lo + 1 : pn.lo - 1;
      }
    }
    return hex_ok;
  }

  // Kind and size checks for one file.
  bool file_passes(uint32_t e) const {
    const uint8_t v = file_ok[ix.file_kind[e - ix.dirs]];
    return v == 1 || (v == 2 && ix.size(e) >= q.min_size && ix.size(e) <= q.max_size);
  }

  bool name_ok(std::string_view name) const {
    if (!q.exts.empty()) {
      bool any = false;
      for (const auto& x : q.exts) any = any || ends_with_fold(name, x);
      if (!any) return false;
    }
    for (int i = 0; i < int(q.terms.size()); ++i)
      if (i != skip && !contains_fold(name, q.terms[i])) return false;
    return true;
  }

  void all(uint32_t e0, uint32_t e1, Chunk& o) const {
    o.count += e1 - e0;
    for (uint32_t e = e0; e < e1 && o.hits.size() < limit; ++e) o.hits.push_back(e);
  }

  void files(uint32_t e0, uint32_t e1, Chunk& o) const {
    if (!file_filter) return all(e0, e1, o);
    const uint8_t* kind = ix.file_kind.p;
    const uint32_t dirs = ix.dirs;
    const uint64_t lo = q.min_size, hi = q.max_size;
    uint64_t count = 0;
    auto test = [&](uint32_t e) {
      const uint8_t v = file_ok[kind[e - dirs]];
      if (v == 0 || (v == 2 && (ix.size(e) < lo || ix.size(e) > hi))) return;
      ++count;
      if (o.hits.size() < limit) o.hits.push_back(e);
    };
    uint32_t e = e0;
#if defined(__ARM_NEON)
    // file_ok[k] == cat_ok[k & 15] & class_ok[k >> 4], so 16 kinds are
    // classified with two table lookups and mostly skipped as a group.
    const uint8x16_t cat_t = vld1q_u8(cat_ok.data()), cls_t = vld1q_u8(class_ok.data());
    for (; e + 16 <= e1; e += 16) {
      const uint8x16_t k = vld1q_u8(kind + (e - dirs));
      const uint8x16_t v = vandq_u8(vqtbl1q_u8(cat_t, vandq_u8(k, vdupq_n_u8(15))), vqtbl1q_u8(cls_t, vshrq_n_u8(k, 4)));
      if (vmaxvq_u8(v) == 0) continue;
      for (uint32_t j = 0; j < 16; ++j) test(e + j);
    }
#endif
    for (; e < e1; ++e) test(e);
    o.count += count;
  }

  // Tests every name in text block `b` of `s`.
  void block(const Section& s, bool file, uint32_t b, Chunk& o) const {
    scan_text_block(
        s, b, needle, [&](std::string_view n) { return name_ok(n); },
        [&](uint32_t e0, uint32_t e1) { file ? files(e0, e1, o) : all(e0, e1, o); });
  }

  // Tests every name in hex block `b` of `s` without decoding it: the needle
  // is matched as nibbles at both alignments, then checked against each
  // name's digit range.
  template <typename Take>
  void hex_block(const Section& s, uint32_t b, char* buf, Take&& take) const {
    const uint32_t u0 = b * kBlock;
    const uint32_t un = std::min(kBlock, s.names() - u0);
    const uint8_t* bs = reinterpret_cast<const uint8_t*>(s.bytes.p + s.block_off[b]);
    const uint8_t* be = reinterpret_cast<const uint8_t*>(s.bytes.p + s.block_off[b + 1]);
    const uint16_t* off = s.name_off.p + u0;
    uint64_t found = 0;  // bit per name in the block
    scan_packed(bs, be, packed, [&](const uint8_t* c, const PackedNeedle& pn) {
      const uint32_t at = uint32_t(c - bs);
      const uint32_t r = uint32_t(std::upper_bound(off, off + un, at) - off) - 1;
      const int64_t nib = 2 * int64_t(at) + pn.align - 2 * (int64_t(off[r]) + 1);
      if (nib >= 0 && nib + int64_t(needle.size()) <= bs[off[r]]) found |= uint64_t(1) << r;
    });
    if (!found) return;
    uint32_t e = s.block_first[b];
    for (uint32_t i = 0; i < un; ++i) {
      const uint32_t c = s.count(u0 + i);
      if ((found >> i) & 1) {
        bool ok = true;
        if (q.terms.size() > 1) {
          size_t len;
          unpack_hex(s.bytes.p + s.block_off[b] + off[i], buf, len);
          ok = name_ok({buf, len});
        }
        if (ok) take(e, e + c);
      }
      e += c;
    }
  }

  void scan(const Section& s, bool file, uint32_t b, Chunk& o, char* buf) const {
    if (!s.hex) return block(s, file, b, o);
    hex_block(s, b, buf, [&](uint32_t e0, uint32_t e1) { file ? files(e0, e1, o) : all(e0, e1, o); });
  }
};

// Whether files need per-file kind or size checks.
bool file_filter(const Query& q) { return q.has_size || (q.types & kTypeFiles) != kTypeFiles; }

struct Job {
  int section;   // 0-3; each job stays inside one section
  bool entries;  // [a, b) are entries (filters only) rather than name blocks
  uint32_t a, b;
};

// Merges two name-ordered result lists (a text and a hex section of the same
// set) into the first `limit` results in name order.
std::vector<uint32_t> merge_by_name(const Index& ix, const std::vector<uint32_t>& a,
                                    const std::vector<uint32_t>& b, size_t limit) {
  std::vector<uint32_t> out;
  size_t i = 0, j = 0;
  std::string na, nb;
  bool have_a = false, have_b = false;
  while (out.size() < limit && i < a.size() && j < b.size()) {
    if (!have_a) na = ix.name(a[i]), have_a = true;
    if (!have_b) nb = ix.name(b[j]), have_b = true;
    if (na <= nb) {
      out.push_back(a[i++]);
      have_a = false;
    } else {
      out.push_back(b[j++]);
      have_b = false;
    }
  }
  // Once one side is exhausted the rest needs no comparisons.
  for (; out.size() < limit && i < a.size(); ++i) out.push_back(a[i]);
  for (; out.size() < limit && j < b.size(); ++j) out.push_back(b[j]);
  return out;
}

// First `limit` hits per section, folders before files, each in name order.
std::vector<uint32_t> collect(const Index& ix, const std::vector<Job>& jobs, const std::vector<Chunk>& out,
                              size_t limit) {
  std::array<std::vector<uint32_t>, 4> per;
  for (size_t j = 0; j < jobs.size(); ++j)
    for (uint32_t e : out[j].hits) {
      auto& v = per[size_t(jobs[j].section)];
      if (v.size() >= limit) break;
      v.push_back(e);
    }
  std::vector<uint32_t> top = merge_by_name(ix, per[0], per[1], limit);
  if (top.size() < limit) {
    const auto files = merge_by_name(ix, per[2], per[3], limit - top.size());
    top.insert(top.end(), files.begin(), files.end());
  }
  return top;
}

uint64_t parse_size(std::string_view s, bool& ok) {
  size_t i = 0;
  double v = 0;
  bool digits = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, digits = true) v = v * 10 + (s[i] - '0');
  if (i < s.size() && s[i] == '.') {
    double scale = 0.1;
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, scale /= 10, digits = true)
      v += (s[i] - '0') * scale;
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

std::string fold(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(kFold[uint8_t(c)]);
  return out;
}

}  // namespace

Query parse_query(std::string_view text) {
  Query q;
  size_t i = 0;
  auto bad = [&](std::string_view tok) {
    if (q.error.empty()) q.error = "ignored " + std::string(tok);
  };
  while (i < text.size()) {
    while (i < text.size() && text[i] == ' ') ++i;
    if (i >= text.size()) break;
    std::string tok;
    if (text[i] == '"') {
      size_t j = text.find('"', i + 1);
      if (j == std::string_view::npos) j = text.size();
      tok = text.substr(i + 1, j - i - 1);
      i = j + 1;
      if (!tok.empty()) q.terms.push_back(fold(tok));
      continue;
    }
    size_t j = text.find(' ', i);
    if (j == std::string_view::npos) j = text.size();
    tok = text.substr(i, j - i);
    i = j;

    std::string lower = fold(tok);
    std::string_view t = lower;
    if (t.starts_with("size:")) {
      std::string_view v = t.substr(5);
      int op = 0;  // 0: >=, 1: >, 2: <, 3: <=, 4: =
      if (v.starts_with(">=")) op = 0, v.remove_prefix(2);
      else if (v.starts_with("<=")) op = 3, v.remove_prefix(2);
      else if (v.starts_with(">")) op = 1, v.remove_prefix(1);
      else if (v.starts_with("<")) op = 2, v.remove_prefix(1);
      else if (v.starts_with("=")) op = 4, v.remove_prefix(1);
      bool ok = false;
      uint64_t n = parse_size(v, ok);
      if (!ok) { bad(tok); continue; }
      q.has_size = true;
      if (op == 0 || op == 4) q.min_size = std::max(q.min_size, n);
      if (op == 1) q.min_size = std::max(q.min_size, n + 1);
      if (op == 2 && n == 0) q.min_size = UINT64_MAX, q.max_size = 0;  // size:<0 matches nothing
      if (op == 2 && n > 0) q.max_size = std::min(q.max_size, n - 1);
      if (op == 3 || op == 4) q.max_size = std::min(q.max_size, n);
    } else if (t.starts_with("type:")) {
      uint32_t any = 0;
      for (auto p : split(t.substr(5), ',')) {
        const uint32_t bits = type_bits(p);
        if (!bits) bad(tok);
        any |= bits;
      }
      if (any) q.types &= any;
      else bad(tok);
    } else if (t.starts_with("sym:")) {
      if (t.size() > 4) q.syms.push_back(std::string(t.substr(4)));
    } else if (t.starts_with("ext:")) {
      auto parts = split(t.substr(4), ',');
      if (parts.empty()) bad(tok);
      for (auto p : parts) q.exts.push_back("." + std::string(p.starts_with('.') ? p.substr(1) : p));
    } else {
      q.terms.push_back(lower);
    }
  }
  return q;
}

Results Engine::search(const Index& ix, const Query& q, size_t limit) {
  Results r;
  if (ix.count() == 0) return r;

  Ctx ctx{ix, q, limit, {}, -1, file_filter(q), {}, {}, {}, {}};
  ctx.init_file_ok();

  const bool by_name = !q.terms.empty() || !q.exts.empty();
  const bool hex_ok = ctx.init_names();

  const bool dirs_ok = (q.types & kTypeDir) && !q.has_size;  // a size filter implies files
  const bool files_ok = (q.types & kTypeFiles) != 0;
  const size_t target = size_t(pool_.size()) * 24;
  std::vector<Job> jobs;
  if (!by_name) {
    for (int s = 0; s < 2 && dirs_ok; ++s) {
      const uint32_t a = std::max(1u, ix.sections[size_t(s)].first_entry()), b = ix.sections[size_t(s)].end_entry();
      if (a < b) jobs.push_back({s, true, a, b});  // all directories but the root
    }
    for (int s = 2; s < 4 && files_ok; ++s) {
      const uint32_t first = ix.sections[size_t(s)].first_entry(), count = ix.sections[size_t(s)].end_entry() - first;
      const size_t parts = std::max<size_t>(1, target * count / std::max<uint32_t>(1, ix.n - ix.dirs));
      for (size_t j = 0; j < parts; ++j) {
        const uint32_t a = first + uint32_t(count * j / parts), b = first + uint32_t(count * (j + 1) / parts);
        if (a < b) jobs.push_back({s, true, a, b});
      }
    }
  } else {
    auto wanted = [&](int s) { return (s < 2 ? dirs_ok : files_ok) && (s % 2 == 0 || hex_ok); };
    size_t bytes = 0;
    for (int s = 0; s < 4; ++s)
      if (wanted(s)) bytes += ix.sections[size_t(s)].block_off[ix.sections[size_t(s)].blocks()];
    const size_t per_job = std::max<size_t>(1, bytes / target);
    for (int s = 0; s < 4; ++s)
      if (wanted(s))
        split_blocks(ix.sections[size_t(s)], per_job, [&](uint32_t b, uint32_t e) { jobs.push_back({s, false, b, e}); });
  }

  // One- and two-character queries match most of the disk (they are the
  // first keystrokes). Their totals are precomputed, so only fill the screen.
  if (by_name && limit <= 1000 && q.terms.size() == 1 && q.exts.empty() && !ctx.file_filter &&
      q.terms[0].size() <= 2) {
    const size_t slot = gram_slot(q.terms[0]);
    const uint64_t total = (dirs_ok ? ix.grams[slot] : 0) + (files_ok ? ix.grams[kGramSlots + slot] : 0);
    if (total >= 50000) {
      // Each section until it has a screenful, then merge in name order.
      std::vector<Chunk> out(jobs.size());
      std::array<size_t, 4> found{};
      std::array<char, 256 + 32> buf;
      for (size_t j = 0; j < jobs.size(); ++j) {
        const int sec = jobs[j].section;
        const Section& s = ix.sections[size_t(sec)];
        for (uint32_t b = jobs[j].a; b < jobs[j].b && found[size_t(sec)] + out[j].hits.size() < limit; ++b)
          ctx.scan(s, sec >= 2, b, out[j], buf.data());
        found[size_t(sec)] += out[j].hits.size();
      }
      r.total = total;
      r.top = collect(ix, jobs, out, limit);
      return r;
    }
  }

  std::vector<Chunk> out(jobs.size());
  pool_.run(jobs.size(), [&](size_t j) {
    std::array<char, 256 + 32> buf;
    const Job& job = jobs[j];
    if (job.entries) return job.section < 2 ? ctx.all(job.a, job.b, out[j]) : ctx.files(job.a, job.b, out[j]);
    const Section& s = ix.sections[size_t(job.section)];
    for (uint32_t b = job.a; b < job.b; ++b) ctx.scan(s, job.section >= 2, b, out[j], buf.data());
  });

  for (auto& o : out) r.total += o.count;
  r.top = collect(ix, jobs, out, limit);
  return r;
}

Results Engine::search_symbols(const Index& ix, const SymbolIndex& sx, const Query& q, size_t limit) {
  Results r;
  if (q.syms.empty() || !(q.types & kTypeFiles)) return r;

  // Symbol names must contain every sym: term; other parts of the query
  // filter the files that define them.
  std::string_view needle;
  int skip = -1;
  for (int i = 0; i < int(q.syms.size()); ++i)
    if (q.syms[i].size() > needle.size()) needle = q.syms[i], skip = i;
  auto sym_ok = [&](std::string_view name) {
    for (int i = 0; i < int(q.syms.size()); ++i)
      if (i != skip && !contains_fold(name, q.syms[i])) return false;
    return true;
  };
  Ctx files{ix, q, limit, {}, -1, file_filter(q), {}, {}, {}, {}};
  files.init_file_ok();
  const bool by_name = !q.terms.empty() || !q.exts.empty();
  const bool by_file = files.file_filter || by_name;

  // File name filters: mark matching files once with a normal file scan, so
  // each definition costs a bit test instead of a name lookup.
  std::vector<uint64_t> named;
  if (by_name) {
    const bool hex_ok = files.init_names();
    named.assign((ix.count() + 63) / 64, 0);
    std::vector<std::pair<int, std::pair<uint32_t, uint32_t>>> fjobs;
    size_t bytes = 0;
    for (int s = 2; s < 4; ++s)
      if (s == 2 || hex_ok) bytes += ix.sections[size_t(s)].block_off[ix.sections[size_t(s)].blocks()];
    const size_t fper = std::max<size_t>(1, bytes / (size_t(pool_.size()) * 24));
    for (int s = 2; s < 4; ++s)
      if (s == 2 || hex_ok)
        split_blocks(ix.sections[size_t(s)], fper, [&](uint32_t b, uint32_t e) { fjobs.push_back({s, {b, e}}); });
    pool_.run(fjobs.size(), [&](size_t j) {
      std::array<char, 256 + 32> buf;
      const Section& sec = ix.sections[size_t(fjobs[j].first)];
      auto mark = [&](uint32_t e0, uint32_t e1) {
        for (uint32_t e = e0; e < e1; ++e) __atomic_fetch_or(&named[e / 64], uint64_t(1) << (e % 64), __ATOMIC_RELAXED);
      };
      for (uint32_t b = fjobs[j].second.first; b < fjobs[j].second.second; ++b) {
        if (sec.hex) {
          files.hex_block(sec, b, buf.data(), mark);
        } else {
          scan_text_block(sec, b, files.needle, [&](std::string_view n) { return files.name_ok(n); }, mark);
        }
      }
    });
  }

  std::vector<std::pair<uint32_t, uint32_t>> jobs;
  const size_t per_job = std::max<size_t>(1, sx.names.block_off[sx.names.blocks()] / (size_t(pool_.size()) * 24));
  split_blocks(sx.names, per_job, [&](uint32_t b, uint32_t e) { jobs.emplace_back(b, e); });

  std::vector<Chunk> out(jobs.size());
  pool_.run(jobs.size(), [&](size_t j) {
    Chunk& o = out[j];
    auto take = [&](uint32_t o0, uint32_t o1) {
      if (!by_file) return files.all(o0, o1, o);
      for (uint32_t i = o0; i < o1; ++i) {
        const uint32_t e = sx.occ[i];
        if (files.file_filter && !files.file_passes(e)) continue;
        if (by_name && !((named[e / 64] >> (e % 64)) & 1)) continue;
        ++o.count;
        if (o.hits.size() < limit) o.hits.push_back(i);
      }
    };
    for (uint32_t b = jobs[j].first; b < jobs[j].second; ++b) scan_text_block(sx.names, b, needle, sym_ok, take);
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
