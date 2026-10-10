#include "symbols/symbols.hpp"
#include "symbols/lexer.hpp"

#include <unordered_set>

namespace fplussearch {

using symbols_detail::T;
using symbols_detail::Tok;
using symbols_detail::lex;

namespace {

const std::unordered_set<std::string_view>& control_words() {
  static const std::unordered_set<std::string_view> w = {
      "if", "for", "while", "switch", "catch", "return", "sizeof", "alignof", "decltype", "typeof",
      "defined", "__attribute__", "__declspec", "do", "else", "foreach", "lock", "fixed", "using",
      "unchecked", "checked", "synchronized", "try", "function", "elif", "when", "with", "until",
      "unless", "case", "new", "delete", "throw", "assert", "static_assert", "_Static_assert",
      "offsetof", "noexcept", "requires", "operator", "template", "typename", "alignas", "asm",
      "__asm__", "va_arg", "await", "yield", "in", "is", "as", "super", "this", "nameof", "default"};
  return w;
}

// Identifiers that can precede a function name but are not a return type.
const std::unordered_set<std::string_view>& non_type_words() {
  static const std::unordered_set<std::string_view> w = {
      "return", "else", "new", "delete", "throw", "case", "goto", "typeof", "sizeof", "await",
      "yield", "co_return", "co_await", "in", "is", "as", "and", "or", "not", "do"};
  return w;
}

const std::unordered_set<std::string_view>& js_method_prefix() {
  static const std::unordered_set<std::string_view> w = {
      "async", "static", "get", "set", "public", "private", "protected", "readonly", "override", "abstract"};
  return w;
}

class Parser {
 public:
  Parser(std::string_view s, const std::vector<Tok>& t, Lang lang, std::vector<Symbol>& out)
      : s_(s), t_(t), n_(t.size()), lang_(lang), out_(out) {
    cfam_ = lang == Lang::C || lang == Lang::Cpp || lang == Lang::ObjC || lang == Lang::Java || lang == Lang::CSharp;
    cish_ = lang == Lang::C || lang == Lang::Cpp || lang == Lang::ObjC;
    js_ = lang == Lang::JS || lang == Lang::TS;
    objc_ = lang == Lang::ObjC ||
            (cish_ && (s.find("@interface") != std::string_view::npos ||
                       s.find("@implementation") != std::string_view::npos));
  }

  void run() {
    for (size_t i = 0; i < n_; ++i) {
      const Tok& k = t_[i];
      if (k.type == T::Macro) {
        emit(i, SymKind::Macro);
        continue;
      }
      if (k.type == T::Punct) {
        // Objective-C method: "- (type)name" at line start.
        if (objc_ && k.bol && (p(i, '-') || p(i, '+')) && p(i + 1, '(')) {
          const size_t c = match(i + 1);
          if (id(c + 1)) emit(c + 1, SymKind::Function);
        }
        continue;
      }
      if (k.type != T::Ident) continue;
      const bool member = i > 0 && (p(i - 1, '.') || p2(i - 1, "->"));
      if (!member && keyword(i)) continue;
      if (cfam_ || js_) {
        size_t body = 0;
        if (!member && c_function(i, body)) {
          emit(i, SymKind::Function);
          if (cfam_ && body > i) i = body;  // never look inside function bodies
          continue;
        }
      }
      if (js_ && !member && p(i + 1, '=') && !p(i + 2, '=') && statement_start(i) && js_function_value(i + 2))
        emit(i, SymKind::Function);
      if (lang_ == Lang::Shell && k.bol && p(i + 1, '(') && p(i + 2, ')')) emit(i, SymKind::Function);
    }
  }

 private:
  std::string_view text(size_t i) const { return s_.substr(t_[i].off, t_[i].len); }
  bool id(size_t i) const { return i < n_ && t_[i].type == T::Ident; }
  bool p(size_t i, char c) const { return i < n_ && t_[i].type == T::Punct && t_[i].len == 1 && s_[t_[i].off] == c; }
  bool p2(size_t i, const char* two) const {
    return i < n_ && t_[i].type == T::Punct && t_[i].len == 2 && s_[t_[i].off] == two[0] && s_[t_[i].off + 1] == two[1];
  }
  bool is(size_t i, std::string_view w) const { return id(i) && text(i) == w; }

  void emit(size_t i, SymKind kind) {
    if (!id(i)) return;
    const std::string_view w = text(i);
    if (w.empty() || w.size() > 128 || w[0] == '$' || uint8_t(w[0]) - '0' < 10u || w == "operator") return;
    out_.push_back({w, t_[i].line, kind});
  }

  // Index of the bracket closing the one at i, or n_ - 1.
  size_t match(size_t i) const {
    if (i >= n_) return n_;
    const char open = s_[t_[i].off];
    const char close = open == '(' ? ')' : open == '{' ? '}' : open == '[' ? ']' : '>';
    int depth = 0;
    for (size_t j = i; j < n_; ++j) {
      if (p(j, open)) ++depth;
      else if (p(j, close) && --depth == 0) return j;
    }
    return n_ - 1;
  }

  // Skips a <...> generic argument list starting at i (bounded).
  size_t skip_angles(size_t i) const {
    int depth = 0;
    for (size_t j = i; j < n_ && j < i + 64; ++j) {
      if (p(j, '<')) ++depth;
      else if (p(j, '>') && --depth == 0) return j;
      else if (p(j, ';') || p(j, '{')) return i;
    }
    return i;
  }

  bool statement_start(size_t i) const {
    if (i == 0 || t_[i].bol) return true;
    return p(i - 1, '{') || p(i - 1, '}') || p(i - 1, ';');
  }

  // name(args) [qualifiers / return type] { ... }
  bool c_function(size_t i, size_t& body) const {
    if (!p(i + 1, '(') || control_words().count(text(i))) return false;
    if (i > 0) {
      const Tok& pv = t_[i - 1];
      bool ok;
      if (cfam_) {
        ok = (pv.type == T::Ident && !non_type_words().count(text(i - 1))) || p(i - 1, '*') || p(i - 1, '&') ||
             p(i - 1, '>') || p(i - 1, '~') || p(i - 1, ';') || p(i - 1, '}') || p(i - 1, '{') ||
             p(i - 1, ']') || p2(i - 1, "::");
      } else {
        ok = p(i - 1, '{') || p(i - 1, '}') || p(i - 1, ';') || p(i - 1, ',') || p(i - 1, '*') ||
             (pv.type == T::Ident && js_method_prefix().count(text(i - 1))) || t_[i].bol;
      }
      if (!ok) return false;
    }
    const size_t close = match(i + 1);
    if (close + 1 >= n_ || !p(close, ')')) return false;
    int depth = 0;
    bool init_list = false;
    for (size_t j = close + 1, steps = 0; j < n_ && steps < 64; ++j, ++steps) {
      if (p(j, '(') || p(j, '[')) {
        ++depth;
        continue;
      }
      if (p(j, ')') || p(j, ']')) {
        if (depth-- == 0) return false;
        continue;
      }
      if (depth) continue;
      if (p(j, '{')) {
        body = match(j);
        return true;
      }
      if (p(j, ':') && cish_) init_list = true;
      if (p(j, ';') || p(j, '=') || p(j, '}') || (p(j, ',') && !init_list)) return false;
      if (p2(j, "=>")) {
        body = 0;
        return lang_ == Lang::CSharp;
      }
    }
    return false;
  }

  bool js_function_value(size_t j) const {
    if (is(j, "async")) ++j;
    if (is(j, "function")) return true;
    if (id(j) && p2(j + 1, "=>")) return true;
    if (!p(j, '(')) return false;
    size_t k = match(j) + 1;
    if (p(k, ':'))  // TypeScript return type
      for (size_t steps = 0; k < n_ && steps < 32 && !p2(k, "=>") && !p(k, ';') && !p(k, '{'); ++k, ++steps) {}
    return p2(k, "=>");
  }

  // Name after a type keyword, accepting qualified names, attributes and
  // templates; requires a body or base list to follow (not a declaration).
  void c_type(size_t i, SymKind kind) {
    size_t j = i + 1, name = n_;
    if (text(i) == "enum" && (is(j, "class") || is(j, "struct"))) ++j;
    for (size_t steps = 0; j < n_ && steps < 12; ++j, ++steps) {
      if (id(j)) {
        if (text(j) != "final" && text(j) != "sealed") name = j;
        continue;
      }
      if (p2(j, "::")) continue;
      if (p(j, '[') || p(j, '(')) {
        j = match(j);
        continue;
      }
      if (p(j, '<')) {
        const size_t e = skip_angles(j);
        if (e == j) return;
        j = e;
        continue;
      }
      if (p(j, '{') || p(j, ':')) break;
      return;
    }
    if (name < n_) emit(name, kind);
  }

  void typedef_name(size_t i) {
    size_t last = n_, fn = n_;
    int depth = 0;
    for (size_t j = i + 1; j < n_ && j < i + 4096; ++j) {
      if (p(j, '(') || p(j, '{') || p(j, '[')) {
        if (p(j, '(') && p(j + 1, '*') && id(j + 2) && p(j + 3, ')')) fn = j + 2;
        ++depth;
      } else if (p(j, ')') || p(j, '}') || p(j, ']')) {
        --depth;
      } else if (depth == 0 && p(j, ';')) {
        break;
      } else if (depth == 0 && id(j)) {
        last = j;
      }
    }
    emit(fn < n_ ? fn : last, SymKind::Type);
  }

  // Handles language keywords; returns true if text(i) is one.
  bool keyword(size_t i) {
    const std::string_view w = text(i);
    switch (lang_) {
      case Lang::C: case Lang::Cpp: case Lang::ObjC:
        if (w == "struct" || w == "class" || w == "union" || w == "enum" || w == "concept") return c_type(i, SymKind::Type), true;
        if (w == "namespace") return c_type(i, SymKind::Namespace), true;
        if (w == "typedef") return typedef_name(i), true;
        if (w == "using" && id(i + 1) && p(i + 2, '=')) return emit(i + 1, SymKind::Type), true;
        if (objc_ && (w == "interface" || w == "implementation" || w == "protocol") && i > 0 && p(i - 1, '@'))
          return emit(i + 1, SymKind::Type), true;
        return false;
      case Lang::Java:
        if (w == "class" || w == "enum" || w == "record" || w == "interface") return emit(i + 1, SymKind::Type), true;
        return false;
      case Lang::CSharp:
        if (w == "class" || w == "struct" || w == "interface" || w == "enum" || w == "record")
          return emit(i + 1, SymKind::Type), true;
        if (w == "namespace") {
          size_t j = i + 1;
          while (id(j) && p(j + 1, '.')) j += 2;
          return emit(j, SymKind::Namespace), true;
        }
        return false;
      case Lang::Kotlin:
        if (w == "fun") {
          for (size_t j = i + 1; j < n_ && j < i + 24; ++j) {
            if (p(j, '<')) j = skip_angles(j);
            else if (p(j, '(')) return emit(j - 1, SymKind::Function), true;
            else if (p(j, '{') || p(j, '=')) break;
          }
          return true;
        }
        if (w == "class" || w == "interface" || w == "object" || w == "typealias") return emit(i + 1, SymKind::Type), true;
        return false;
      case Lang::Swift:
        if (w == "func") return emit(i + 1, SymKind::Function), true;
        if (w == "class" || w == "struct" || w == "enum" || w == "protocol" || w == "actor" || w == "typealias") {
          if (id(i + 1) && text(i + 1) != "func" && text(i + 1) != "var" && text(i + 1) != "let")
            emit(i + 1, SymKind::Type);
          return true;
        }
        return false;
      case Lang::Rust:
        if (w == "fn") return emit(i + 1, SymKind::Function), true;
        if (w == "struct" || w == "enum" || w == "trait" || w == "type") return emit(i + 1, SymKind::Type), true;
        if (w == "union" && id(i + 1) && (p(i + 2, '{') || p(i + 2, '<'))) return emit(i + 1, SymKind::Type), true;
        if (w == "mod") return emit(i + 1, SymKind::Namespace), true;
        if (w == "const" || w == "static") {
          size_t j = i + 1;
          if (is(j, "mut")) ++j;
          if (id(j) && text(j) != "_" && p(j + 1, ':')) emit(j, SymKind::Constant);
          return true;
        }
        if (w == "macro_rules" && p(i + 1, '!')) return emit(i + 2, SymKind::Macro), true;
        return false;
      case Lang::Go:
        if (w == "func") {
          size_t j = i + 1;
          if (p(j, '(')) j = match(j) + 1;
          return emit(j, SymKind::Function), true;
        }
        if (w == "type") {
          if (id(i + 1)) return emit(i + 1, SymKind::Type), true;
          if (p(i + 1, '(')) {
            const size_t close = match(i + 1);
            int depth = 0;
            for (size_t j = i + 2; j < close; ++j) {
              if (p(j, '{') || p(j, '(') || p(j, '[')) ++depth;
              else if (p(j, '}') || p(j, ')') || p(j, ']')) --depth;
              else if (depth == 0 && t_[j].bol && id(j)) emit(j, SymKind::Type);
            }
          }
          return true;
        }
        return false;
      case Lang::Python:
        if (w == "def" || w == "class") {
          if (t_[i].bol || (i > 0 && is(i - 1, "async"))) emit(i + 1, w == "def" ? SymKind::Function : SymKind::Type);
          return true;
        }
        return false;
      case Lang::JS: case Lang::TS: {
        if (w == "function") {
          size_t j = i + 1;
          if (p(j, '*')) ++j;
          return emit(j, SymKind::Function), true;
        }
        if (w == "class") {
          if (id(i + 1) && text(i + 1) != "extends" && text(i + 1) != "implements") emit(i + 1, SymKind::Type);
          return true;
        }
        if (w == "const" || w == "let" || w == "var") {
          if (id(i + 1) && p(i + 2, '=') && js_function_value(i + 3)) emit(i + 1, SymKind::Function);
          return true;
        }
        if (lang_ != Lang::TS) return false;
        if (w == "interface" || w == "enum") return emit(i + 1, SymKind::Type), true;
        if (w == "type" && id(i + 1) && (p(i + 2, '=') || p(i + 2, '<'))) return emit(i + 1, SymKind::Type), true;
        if ((w == "namespace" || w == "module") && id(i + 1) && (p(i + 2, '{') || p(i + 2, '.')))
          return emit(i + 1, SymKind::Namespace), true;
        return false;
      }
      case Lang::Ruby:
        if (w == "def") {
          size_t j = i + 1;
          if (is(j, "self") && p(j + 1, '.')) j += 2;
          return emit(j, SymKind::Function), true;
        }
        if (w == "class" || w == "module") {
          size_t j = i + 1;
          while (id(j) && p2(j + 1, "::")) j += 2;
          if (id(j)) emit(j, w == "class" ? SymKind::Type : SymKind::Namespace);
          return true;
        }
        return false;
      case Lang::PHP:
        if (w == "function") {
          size_t j = i + 1;
          if (p(j, '&')) ++j;
          return emit(j, SymKind::Function), true;
        }
        if (w == "class" || w == "interface" || w == "trait" || w == "enum") return emit(i + 1, SymKind::Type), true;
        return false;
      case Lang::Lua:
        if (w == "function") {
          size_t j = i + 1, name = n_;
          while (id(j)) {
            name = j;
            if (p(j + 1, '.') || p(j + 1, ':')) j += 2;
            else break;
          }
          if (name < n_ && p(name + 1, '(')) emit(name, SymKind::Function);
          return true;
        }
        return false;
      case Lang::Zig:
        if (w == "fn") return emit(i + 1, SymKind::Function), true;
        if (w == "const" && id(i + 1) && p(i + 2, '=')) {
          size_t j = i + 3;
          while (is(j, "extern") || is(j, "packed")) ++j;
          if (is(j, "struct") || is(j, "enum") || is(j, "union") || is(j, "opaque")) emit(i + 1, SymKind::Type);
          return true;
        }
        return false;
      case Lang::Shell:
        if (w == "function") return emit(i + 1, SymKind::Function), true;
        return false;
      case Lang::None:
        return false;
    }
    return false;
  }

  std::string_view s_;
  const std::vector<Tok>& t_;
  size_t n_;
  Lang lang_;
  std::vector<Symbol>& out_;
  bool cfam_ = false, cish_ = false, js_ = false, objc_ = false;
};

}  // namespace

void extract_symbols(std::string_view src, Lang lang, std::vector<Symbol>& out) {
  if (lang == Lang::None) return;
  thread_local std::vector<Tok> toks;
  toks.clear();
  lex(src, lang, toks);
  const size_t first = out.size();
  Parser(src, toks, lang, out).run();
  // Keep each name once, at its first definition.
  thread_local std::unordered_set<std::string_view> seen;
  seen.clear();
  size_t w = first;
  for (size_t r = first; r < out.size(); ++r)
    if (seen.insert(out[r].name).second) out[w++] = out[r];
  out.resize(w);
}

}  // namespace fplussearch
