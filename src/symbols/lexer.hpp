#pragma once

#include "symbols/symbols.hpp"

namespace fplussearch::symbols_detail {

enum class T : uint8_t { Ident, Num, Str, Punct, Macro };

struct Tok {
  uint32_t off;
  uint32_t line;
  uint16_t len;
  T type;
  bool bol;  // first token on its line
};

void lex(std::string_view source, Lang lang, std::vector<Tok>& tokens);

}  // namespace fplussearch::symbols_detail
