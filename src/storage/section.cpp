#include "storage/section.hpp"

namespace fplussearch {

bool is_hex_name(std::string_view s) {
  if (s.size() < 16 || s.size() > 255) return false;
  for (char c : s)
    if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
  return true;
}

void pack_hex(std::string_view s, BigVec<char>& out) {
  auto val = [](char c) { return c <= '9' ? c - '0' : c - 'a' + 10; };
  out.push_back(char(uint8_t(s.size())));
  for (size_t i = 0; i < s.size(); i += 2)
    out.push_back(char((val(s[i]) << 4) | (i + 1 < s.size() ? val(s[i + 1]) : 0)));
}

}  // namespace fplussearch
