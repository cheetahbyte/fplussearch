#include "index/index.hpp"

#include <algorithm>

#include "storage/fields.hpp"

namespace fplussearch {

uint64_t Index::big_size(uint32_t e) const {
  const Overflow* o = find_key(big_sizes, e);
  return o ? o->value : UINT32_MAX;
}

std::string Index::name(uint32_t e) const {
  for (const Section& s : sections) {
    if (e < s.first_entry() || e >= s.end_entry()) continue;
    const uint32_t b = uint32_t(std::upper_bound(s.block_first.p, s.block_first.p + s.block_first.n, e) -
                                s.block_first.p - 1);
    uint32_t i = b * kBlock, acc = s.block_first[b];
    while (acc + s.count(i) <= e) acc += s.count(i++);
    const char* p = s.bytes.p + s.block_off[b] + s.name_off[i];
    if (!s.hex) return std::string(p);
    char buf[256 + 32];
    size_t len;
    unpack_hex(p, buf, len);
    return std::string(buf, len);
  }
  return {};
}

std::string_view Index::name_view(uint32_t e, char* buf) const {
  for (const Section& s : sections) {
    if (e < s.first_entry() || e >= s.end_entry()) continue;
    const uint32_t b = uint32_t(std::upper_bound(s.block_first.p, s.block_first.p + s.block_first.n, e) -
                                s.block_first.p - 1);
    uint32_t i = b * kBlock, acc = s.block_first[b];
    while (acc + s.count(i) <= e) acc += s.count(i++);
    const char* p = s.bytes.p + s.block_off[b] + s.name_off[i];
    if (!s.hex) return p;
    size_t len;
    unpack_hex(p, buf, len);
    return {buf, len};
  }
  return {};
}

uint32_t Index::child(uint32_t d, std::string_view name, bool any_case) const {
  // Children are ascending entry ids, and within each section entry ids
  // follow name order: binary search each section's run.
  const Span<uint32_t> k = kids(d);
  char buf[256 + 32];
  for (const Section& s : sections) {
    const uint32_t* lo = std::lower_bound(k.begin(), k.end(), s.first_entry());
    const uint32_t* hi = std::lower_bound(lo, k.end(), s.end_entry());
    const uint32_t* at = std::lower_bound(lo, hi, name, [&](uint32_t e, std::string_view n) { return name_view(e, buf) < n; });
    if (at != hi && name_view(*at, buf) == name) return *at;
  }
  if (any_case)
    for (uint32_t e : k) {
      const std::string_view nm = name_view(e, buf);
      if (nm.size() == name.size() &&
          std::equal(nm.begin(), nm.end(), name.begin(), [](char a, char b) { return kFold[uint8_t(a)] == kFold[uint8_t(b)]; }))
        return e;
    }
  return UINT32_MAX;
}

uint32_t Index::lookup(std::string_view p, bool any_case) const {
  const std::string_view r = root == "/" ? "" : std::string_view(root);
  if (!p.starts_with(r) || (p.size() > r.size() && p[r.size()] != '/')) return UINT32_MAX;
  p.remove_prefix(r.size());
  uint32_t e = 0;
  while (!p.empty()) {
    while (p.starts_with('/')) p.remove_prefix(1);
    if (p.empty()) break;
    const size_t slash = p.find('/');
    const std::string_view comp = p.substr(0, slash);
    p = slash == std::string_view::npos ? std::string_view() : p.substr(slash);
    if (!is_dir(e)) return UINT32_MAX;
    e = child(e, comp, any_case);
    if (e == UINT32_MAX) return e;
  }
  return e;
}

std::string Index::path(uint32_t e) const {
  if (e == 0) return root;
  uint32_t chain[1024];
  int depth = 0;
  for (uint32_t c = e; c != 0 && depth < 1024; c = parent(c)) chain[depth++] = c;
  std::string s = root == "/" ? "" : root;
  for (int j = depth - 1; j >= 0; --j) {
    s += '/';
    s += name(chain[j]);
  }
  return s;
}

}  // namespace fplussearch
