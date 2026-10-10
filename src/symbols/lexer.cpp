#include "symbols/lexer.hpp"

#include <algorithm>
#include <string>

namespace fplussearch::symbols_detail {

namespace {

struct Rules {
  bool slash = false;     // // and /* */ comments
  bool hash = false;      // # comments
  bool lua = false;       // -- comments, [[ ]] strings
  bool preproc = false;   // # directives at line start
  bool squote = true;     // '...' is a string or char literal
  bool backtick = false;  // `...` is a string
  bool triple = false;    // """ and ''' strings
  bool rust = false;      // lifetimes and raw strings
  bool ruby = false;      // ? and ! method name suffixes
  bool dash = false;      // - inside identifiers (shell function names)
};

Rules rules_for(Lang l) {
  Rules r;
  switch (l) {
    case Lang::C: case Lang::Cpp: case Lang::ObjC: r.slash = r.preproc = true; break;
    case Lang::Java: case Lang::Kotlin: case Lang::CSharp: case Lang::Zig: r.slash = true; break;
    case Lang::Swift: r.slash = r.triple = true; break;
    case Lang::Rust: r.slash = r.rust = true; break;
    case Lang::Go: r.slash = r.backtick = true; break;
    case Lang::JS: case Lang::TS: r.slash = r.backtick = true; break;
    case Lang::PHP: r.slash = r.hash = true; break;
    case Lang::Python: r.hash = r.triple = true; break;
    case Lang::Ruby: r.hash = r.ruby = true; break;
    case Lang::Shell: r.hash = r.backtick = r.dash = true; break;
    case Lang::Lua: r.lua = true; break;
    case Lang::None: break;
  }
  return r;
}

bool ident_start(unsigned char c) { return (c | 0x20) - 'a' < 26u || c == '_' || c == '$' || c >= 0x80; }
bool ident_char(unsigned char c) { return ident_start(c) || c - '0' < 10u; }

// Length of a Lua long bracket opener "[[" / "[==[" at i, or 0.
size_t long_bracket(std::string_view s, size_t i, size_t& level) {
  if (i >= s.size() || s[i] != '[') return 0;
  size_t j = i + 1;
  while (j < s.size() && s[j] == '=') ++j;
  if (j < s.size() && s[j] == '[') {
    level = j - i - 1;
    return j - i + 1;
  }
  return 0;
}

}  // namespace

void lex(std::string_view s, Lang lang, std::vector<Tok>& toks) {
  const Rules r = rules_for(lang);
  const size_t n = s.size();
  size_t i = 0;
  uint32_t line = 1;
  bool bol = true;
  auto push = [&](size_t start, size_t len, T type) {
    toks.push_back({uint32_t(start), line, uint16_t(std::min<size_t>(len, 65535)), type, bol});
    bol = false;
  };
  auto skip_to_eol = [&] {
    while (i < n && s[i] != '\n') ++i;
  };
  auto skip_until = [&](std::string_view close) {
    while (i < n && s.compare(i, close.size(), close) != 0) line += s[i++] == '\n';
    i = std::min(n, i + close.size());
  };

  while (i < n) {
    const unsigned char c = uint8_t(s[i]);
    if (c == '\n') {
      ++line;
      bol = true;
      ++i;
      continue;
    }
    if (c == ' ' || c == '\t' || c == '\r' || c == '\f' || c == '\v') {
      ++i;
      continue;
    }
    if (r.slash && c == '/' && i + 1 < n && s[i + 1] == '/') {
      skip_to_eol();
      continue;
    }
    if (r.slash && c == '/' && i + 1 < n && s[i + 1] == '*') {
      i += 2;
      skip_until("*/");
      continue;
    }
    if (r.preproc && c == '#' && bol) {
      size_t j = i + 1;
      while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
      if (s.compare(j, 6, "define") == 0) {
        j += 6;
        while (j < n && (s[j] == ' ' || s[j] == '\t')) ++j;
        size_t k = j;
        while (k < n && ident_char(uint8_t(s[k]))) ++k;
        if (k > j) push(j, k - j, T::Macro);
      }
      while (i < n && s[i] != '\n') {
        if (s[i] == '\\' && i + 1 < n && (s[i + 1] == '\n' || s[i + 1] == '\r')) {
          i += s[i + 1] == '\r' ? 2 : 1;
          if (i < n && s[i] == '\n') ++line, ++i;
          continue;
        }
        ++i;
      }
      continue;
    }
    if (r.hash && c == '#') {
      skip_to_eol();
      continue;
    }
    if (r.lua && c == '-' && i + 1 < n && s[i + 1] == '-') {
      i += 2;
      size_t level, len = long_bracket(s, i, level);
      if (len) {
        i += len;
        skip_until("]" + std::string(level, '=') + "]");
      } else {
        skip_to_eol();
      }
      continue;
    }
    if (r.lua && c == '[') {
      size_t level, len = long_bracket(s, i, level);
      if (len) {
        const size_t start = i;
        i += len;
        skip_until("]" + std::string(level, '=') + "]");
        push(start, i - start, T::Str);
        continue;
      }
    }
    if (r.rust && c == '\'') {
      // 'x' and '\n' are chars; 'a without a closing quote is a lifetime.
      if (!(i + 2 < n && (s[i + 2] == '\'' || s[i + 1] == '\\'))) {
        ++i;
        continue;
      }
    }
    if (c == '"' || (c == '\'' && r.squote) || (c == '`' && r.backtick)) {
      const size_t start = i;
      if (r.triple && c != '`' && i + 2 < n && s[i + 1] == char(c) && s[i + 2] == char(c)) {
        i += 3;
        skip_until(std::string(3, char(c)));
      } else {
        const bool multiline = c == '`';
        ++i;
        while (i < n && s[i] != char(c)) {
          if (s[i] == '\\' && i + 1 < n) {
            line += s[i + 1] == '\n';
            i += 2;
            continue;
          }
          if (s[i] == '\n') {
            if (!multiline) break;  // unterminated: recover at end of line
            ++line;
          }
          ++i;
        }
        if (i < n && s[i] == char(c)) ++i;
      }
      push(start, i - start, T::Str);
      continue;
    }
    if (ident_start(c)) {
      const size_t start = i;
      ++i;
      while (i < n && (ident_char(uint8_t(s[i])) ||
                       (r.dash && s[i] == '-' && i + 1 < n && ident_char(uint8_t(s[i + 1])))))
        ++i;
      if (r.ruby && i < n && (s[i] == '?' || s[i] == '!') && !(i + 1 < n && s[i + 1] == '=')) ++i;
      const std::string_view w = s.substr(start, i - start);
      if (r.rust && (w == "r" || w == "br") && i < n && (s[i] == '"' || s[i] == '#')) {
        size_t hashes = 0;
        while (i < n && s[i] == '#') ++hashes, ++i;
        if (i < n && s[i] == '"') {
          ++i;
          skip_until("\"" + std::string(hashes, '#'));
          push(start, i - start, T::Str);
          continue;
        }
      }
      push(start, i - start, T::Ident);
      continue;
    }
    if (c - '0' < 10u) {
      const size_t start = i;
      while (i < n && (ident_char(uint8_t(s[i])) || s[i] == '.' || s[i] == '\'')) ++i;
      push(start, i - start, T::Num);
      continue;
    }
    if (i + 1 < n && ((c == ':' && s[i + 1] == ':') || (c == '-' && s[i + 1] == '>') || (c == '=' && s[i + 1] == '>'))) {
      push(i, 2, T::Punct);
      i += 2;
      continue;
    }
    push(i, 1, T::Punct);
    ++i;
  }
}

}  // namespace fplussearch::symbols_detail
