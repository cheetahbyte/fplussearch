#include "symbols/symbols.hpp"

#include <algorithm>
#include <unordered_map>

namespace fplussearch {

Lang lang_of(std::string_view file_name) {
  static const std::unordered_map<std::string_view, Lang> table = {
      {"c", Lang::C},       {"h", Lang::Cpp},      {"cc", Lang::Cpp},    {"cpp", Lang::Cpp},
      {"cxx", Lang::Cpp},   {"c++", Lang::Cpp},    {"hh", Lang::Cpp},    {"hpp", Lang::Cpp},
      {"hxx", Lang::Cpp},   {"m", Lang::ObjC},     {"mm", Lang::ObjC},   {"rs", Lang::Rust},
      {"go", Lang::Go},     {"py", Lang::Python},  {"pyi", Lang::Python}, {"js", Lang::JS},
      {"mjs", Lang::JS},    {"cjs", Lang::JS},     {"jsx", Lang::JS},    {"ts", Lang::TS},
      {"tsx", Lang::TS},    {"mts", Lang::TS},     {"cts", Lang::TS},    {"swift", Lang::Swift},
      {"java", Lang::Java}, {"kt", Lang::Kotlin},  {"kts", Lang::Kotlin}, {"cs", Lang::CSharp},
      {"rb", Lang::Ruby},   {"php", Lang::PHP},    {"lua", Lang::Lua},   {"zig", Lang::Zig},
      {"sh", Lang::Shell},  {"bash", Lang::Shell}, {"zsh", Lang::Shell}, {"fish", Lang::Shell}};
  const size_t dot = file_name.rfind('.');
  if (dot == std::string_view::npos || dot == 0 || file_name.size() - dot > 6) return Lang::None;
  char buf[6];
  const size_t n = file_name.size() - dot - 1;
  for (size_t i = 0; i < n; ++i) buf[i] = char(file_name[dot + 1 + i] | (file_name[dot + 1 + i] >= 'A' && file_name[dot + 1 + i] <= 'Z' ? 0x20 : 0));
  const auto it = table.find(std::string_view(buf, n));
  return it == table.end() ? Lang::None : it->second;
}

const char* kind_name(SymKind k) {
  switch (k) {
    case SymKind::Function: return "fn";
    case SymKind::Type: return "type";
    case SymKind::Macro: return "macro";
    case SymKind::Namespace: return "module";
    case SymKind::Constant: return "const";
  }
  return "";
}

bool looks_minified(std::string_view src) {
  const size_t probe = std::min<size_t>(src.size(), 64 * 1024);
  if (probe < 4096) return false;
  const size_t lines = size_t(std::count(src.begin(), src.begin() + long(probe), '\n')) + 1;
  return probe / lines > 300;
}

}  // namespace fplussearch
