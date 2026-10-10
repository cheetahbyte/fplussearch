#include "content/grams.hpp"

#include <algorithm>

namespace fplussearch::content_detail {

// The doc's distinct case-folded trigrams as (trigram << 8 | follow mask),
// sorted, appended to `out`. `masks` is a 16 MB table over the trigram
// space, all zero on entry and exit.
// Appends the distinct trigrams of buf to `out`, sorted, each as
// trigram << 8 | the follow bits of every character seen after it. Sorting
// a scratch list costs a little CPU but no per-thread 16 MB table.
void trigrams(const char* buf, size_t n, std::vector<uint32_t>& scratch, BigVec<uint32_t>& out) {
  if (n < 3) return;
  scratch.clear();
  uint32_t t = uint32_t(fold(uint8_t(buf[0]))) << 8 | fold(uint8_t(buf[1]));
  for (size_t i = 2; i < n; ++i) {
    t = ((t << 8) | fold(uint8_t(buf[i]))) & 0xffffff;
    scratch.push_back(t << 8 | (i + 1 < n ? follow_bit(fold(uint8_t(buf[i + 1]))) : 0));
  }
  std::sort(scratch.begin(), scratch.end());
  for (size_t i = 0; i < scratch.size();) {
    uint32_t v = scratch[i];
    for (++i; i < scratch.size() && (scratch[i] >> 8) == (v >> 8); ++i) v |= scratch[i] & 0xff;
    out.push_back(v);
  }
}

std::vector<std::pair<uint32_t, uint8_t>> trigrams_small(std::string_view s, bool unicode_caseless) {
  std::vector<std::pair<uint32_t, uint8_t>> t;
  for (size_t i = 0; i + 3 <= s.size(); ++i) {
    if (unicode_caseless && (unicode_case_byte(uint8_t(s[i])) || unicode_case_byte(uint8_t(s[i + 1])) ||
                            unicode_case_byte(uint8_t(s[i + 2])))) continue;
    const uint32_t tri =
        uint32_t(fold(uint8_t(s[i]))) << 16 | uint32_t(fold(uint8_t(s[i + 1]))) << 8 | fold(uint8_t(s[i + 2]));
    const bool follow = i + 3 < s.size() && (!unicode_caseless || !unicode_case_byte(uint8_t(s[i + 3])));
    t.push_back({tri, follow ? follow_bit(fold(uint8_t(s[i + 3]))) : uint8_t(0)});
  }
  std::sort(t.begin(), t.end());
  std::vector<std::pair<uint32_t, uint8_t>> out;
  for (auto& x : t) {
    if (!out.empty() && out.back().first == x.first) out.back().second |= x.second;
    else out.push_back(x);
  }
  return out;
}

}  // namespace fplussearch::content_detail
