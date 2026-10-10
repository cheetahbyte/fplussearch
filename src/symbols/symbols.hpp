#pragma once

#include <cstdint>
#include <string_view>
#include <vector>

namespace fplussearch {

enum class Lang : uint8_t {
  None, C, Cpp, ObjC, Rust, Go, Python, JS, TS, Swift, Java, Kotlin, CSharp, Ruby, PHP, Lua, Zig, Shell
};

enum class SymKind : uint8_t { Function, Type, Macro, Namespace, Constant };

struct Symbol {
  std::string_view name;  // points into the source text
  uint32_t line;          // 1-based
  SymKind kind;
};

Lang lang_of(std::string_view file_name);
const char* kind_name(SymKind k);

// Appends the definitions found in `src` (functions, types, macros, ...),
// each name once. Heuristic: a token scanner with per-language rules, not a
// parser, so unusual code can be missed or misread.
void extract_symbols(std::string_view src, Lang lang, std::vector<Symbol>& out);

// Source that looks generated or minified is not worth scanning.
bool looks_minified(std::string_view src);

}  // namespace fplussearch
