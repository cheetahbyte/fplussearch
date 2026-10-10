#include "content/matcher.hpp"

#include <algorithm>

#include "content/file_io.hpp"

namespace fplussearch::content_detail {

namespace {

bool has_upper(std::string_view s) {
  for (char c : s)
    if (c >= 'A' && c <= 'Z') return true;
  return false;
}

std::string trim_line(std::string_view line) {
  while (!line.empty() && (line.back() == '\r' || line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
  if (line.size() > 200) {
    size_t n = 200;
    while (n > 0 && (uint8_t(line[n]) & 0xc0) == 0x80) --n;  // don't cut a UTF-8 sequence
    line = line.substr(0, n);
  }
  return std::string(line);
}

}  // namespace

// Reads one file fresh and collects up to `per_file` matching lines.
bool match_file(const Matcher& m, const std::string& path, size_t per_file, BigVec<char>& buf, FileMatches& out) {
  if (!read_file(path.c_str(), buf, kMaxContentFile * 4, nullptr, true)) return false;
  const std::string_view s(buf.data(), buf.size());
  size_t from = 0, counted = 0, line_no = 1, last_end = 0;
  bool has_line = false;
  size_t a, b;
  while (out.lines.size() < per_file && from <= s.size() && m.find(s, from, a, b)) {
    if (has_line && a < last_end) {
      from = std::max(last_end + 1, b == a ? a + 1 : b);
      continue;
    }
    for (const char* p = s.data() + counted;
         (p = static_cast<const char*>(std::memchr(p, '\n', size_t(s.data() + a - p))));
         ++p)
      ++line_no;
    counted = a;

    size_t ls = a;
    const size_t bound = has_line ? last_end : 0;
    while (ls > bound && s[ls - 1] != '\n') --ls;

    const void* nl = std::memchr(s.data() + a, '\n', s.size() - a);
    const size_t le = nl ? size_t(static_cast<const char*>(nl) - s.data()) : s.size();

    out.lines.push_back({uint32_t(line_no), trim_line(s.substr(ls, le - ls))});
    last_end = le;
    has_line = true;
    from = std::max(le + 1, b == a ? a + 1 : b);
  }
  if (out.lines.empty()) return false;
  out.path = path;
  return true;
}

bool make_matcher(const GrepOptions& o, Matcher& m, std::string& error) {
  m.regex = o.regex;
  m.caseless = !has_upper(o.pattern);  // smart case
  if (o.pattern.empty()) {
    error = "empty pattern";
    return false;
  }
  if (o.regex) {
    m.re = Regex::compile(o.pattern, m.caseless, true, error);
    return m.re != nullptr;
  }
  m.needle = o.pattern;
  if (m.caseless)
    for (auto& c : m.needle) c = char(fold(uint8_t(c)));
  return true;
}

}  // namespace fplussearch::content_detail
