#pragma once

// Just enough JSON for the daemon's protocol: a small parser, and escaping
// for responses built as text.

#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace fplussearch {

struct JsonValue;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;

struct JsonValue {
  enum Type { Null, Bool, Number, String, Array, Object } type = Null;
  bool b = false;
  double n = 0;
  std::string s;
  std::vector<JsonValue> items;  // Array
  JsonObject fields;             // Object

  const JsonValue* get(std::string_view k) const {
    auto it = fields.find(k);
    return it == fields.end() ? nullptr : &it->second;
  }
  std::string str(std::string_view k) const {
    const JsonValue* v = get(k);
    return v && v->type == String ? v->s : std::string();
  }
  double num(std::string_view k, double d = 0) const {
    const JsonValue* v = get(k);
    return v && v->type == Number ? v->n : d;
  }
  bool flag(std::string_view k) const {
    const JsonValue* v = get(k);
    return v && v->type == Bool && v->b;
  }
};

// Length of the valid UTF-8 sequence at s[i], or 0 if it isn't one.
inline size_t utf8_len(std::string_view s, size_t i) {
  const uint8_t c = uint8_t(s[i]);
  size_t n;
  uint32_t cp;
  if (c < 0x80) return 1;
  if ((c & 0xe0) == 0xc0) n = 2, cp = c & 0x1f;
  else if ((c & 0xf0) == 0xe0) n = 3, cp = c & 0x0f;
  else if ((c & 0xf8) == 0xf0) n = 4, cp = c & 0x07;
  else return 0;
  if (i + n > s.size()) return 0;
  for (size_t k = 1; k < n; ++k) {
    const uint8_t b = uint8_t(s[i + k]);
    if ((b & 0xc0) != 0x80) return 0;
    cp = cp << 6 | (b & 0x3f);
  }
  // Overlong forms, surrogates and out-of-range values aren't valid either.
  if ((n == 2 && cp < 0x80) || (n == 3 && cp < 0x800) || (n == 4 && (cp < 0x10000 || cp > 0x10ffff)) ||
      (cp >= 0xd800 && cp < 0xe000))
    return 0;
  return n;
}

// Appends `s` as a JSON string. Bytes that aren't valid UTF-8 (file names
// and file contents can hold any) become U+FFFD, so the output always is.
inline void json_escape(std::string& out, std::string_view s) {
  out += '"';
  for (size_t i = 0; i < s.size();) {
    const char c = s[i];
    switch (c) {
      case '"': out += "\\\""; ++i; continue;
      case '\\': out += "\\\\"; ++i; continue;
      case '\n': out += "\\n"; ++i; continue;
      case '\r': out += "\\r"; ++i; continue;
      case '\t': out += "\\t"; ++i; continue;
      default: break;
    }
    if (uint8_t(c) < 0x20) {
      char buf[8];
      std::snprintf(buf, sizeof buf, "\\u%04x", unsigned(uint8_t(c)));
      out += buf;
      ++i;
      continue;
    }
    const size_t n = utf8_len(s, i);
    if (n == 0) {
      out += "\xef\xbf\xbd";
      ++i;
    } else {
      out.append(s.data() + i, n);
      i += n;
    }
  }
  out += '"';
}

namespace json_detail {

struct Reader {
  std::string_view in;
  size_t i = 0;
  int depth = 0;

  void ws() {
    while (i < in.size() && (in[i] == ' ' || in[i] == '\t' || in[i] == '\r' || in[i] == '\n')) ++i;
  }

  static void put_utf8(std::string& s, unsigned cp) {
    if (cp < 0x80) {
      s += char(cp);
    } else if (cp < 0x800) {
      s += char(0xc0 | (cp >> 6));
      s += char(0x80 | (cp & 0x3f));
    } else if (cp < 0x10000) {
      s += char(0xe0 | (cp >> 12));
      s += char(0x80 | ((cp >> 6) & 0x3f));
      s += char(0x80 | (cp & 0x3f));
    } else {
      s += char(0xf0 | (cp >> 18));
      s += char(0x80 | ((cp >> 12) & 0x3f));
      s += char(0x80 | ((cp >> 6) & 0x3f));
      s += char(0x80 | (cp & 0x3f));
    }
  }

  bool hex4(unsigned& cp) {
    if (i + 4 >= in.size()) return false;
    cp = 0;
    for (int k = 1; k <= 4; ++k) {
      const char c = in[i + size_t(k)];
      cp <<= 4;
      if (c >= '0' && c <= '9') cp |= unsigned(c - '0');
      else if (c >= 'a' && c <= 'f') cp |= unsigned(c - 'a' + 10);
      else if (c >= 'A' && c <= 'F') cp |= unsigned(c - 'A' + 10);
      else return false;
    }
    i += 4;
    return true;
  }

  bool string(std::string& s) {
    if (i >= in.size() || in[i] != '"') return false;
    for (++i; i < in.size() && in[i] != '"'; ++i) {
      if (in[i] != '\\') {
        s += in[i];
        continue;
      }
      if (++i >= in.size()) return false;
      switch (in[i]) {
        case 'n': s += '\n'; break;
        case 't': s += '\t'; break;
        case 'r': s += '\r'; break;
        case 'b': s += '\b'; break;
        case 'f': s += '\f'; break;
        case 'u': {
          unsigned cp;
          if (!hex4(cp)) return false;
          // A surrogate pair is one character.
          if (cp >= 0xd800 && cp < 0xdc00 && i + 2 < in.size() && in[i + 1] == '\\' && in[i + 2] == 'u') {
            i += 2;
            unsigned lo;
            if (!hex4(lo)) return false;
            cp = 0x10000 + ((cp - 0xd800) << 10) + (lo - 0xdc00);
          }
          put_utf8(s, cp);
          break;
        }
        default: s += in[i];
      }
    }
    if (i >= in.size()) return false;
    ++i;
    return true;
  }

  bool value(JsonValue& v) {
    if (++depth > 64) return false;
    ws();
    if (i >= in.size()) return false;
    const char c = in[i];
    bool ok = true;
    if (c == '"') {
      v.type = JsonValue::String;
      ok = string(v.s);
    } else if (c == '{') {
      v.type = JsonValue::Object;
      ++i;
      ws();
      if (i < in.size() && in[i] == '}') {
        ++i;
      } else {
        for (;;) {
          ws();
          std::string key;
          if (!string(key)) return false;
          ws();
          if (i >= in.size() || in[i] != ':') return false;
          ++i;
          if (!value(v.fields[key])) return false;
          ws();
          if (i < in.size() && in[i] == ',') {
            ++i;
            continue;
          }
          if (i >= in.size() || in[i] != '}') return false;
          ++i;
          break;
        }
      }
    } else if (c == '[') {
      v.type = JsonValue::Array;
      ++i;
      ws();
      if (i < in.size() && in[i] == ']') {
        ++i;
      } else {
        for (;;) {
          v.items.emplace_back();
          if (!value(v.items.back())) return false;
          ws();
          if (i < in.size() && in[i] == ',') {
            ++i;
            continue;
          }
          if (i >= in.size() || in[i] != ']') return false;
          ++i;
          break;
        }
      }
    } else if (in.substr(i).starts_with("true")) {
      v.type = JsonValue::Bool, v.b = true, i += 4;
    } else if (in.substr(i).starts_with("false")) {
      v.type = JsonValue::Bool, i += 5;
    } else if (in.substr(i).starts_with("null")) {
      i += 4;
    } else {
      size_t j = i;
      while (j < in.size() && std::string_view("+-.eE0123456789").find(in[j]) != std::string_view::npos) ++j;
      if (j == i) return false;
      v.type = JsonValue::Number;
      v.n = std::strtod(std::string(in.substr(i, j - i)).c_str(), nullptr);
      i = j;
    }
    --depth;
    return ok;
  }
};

}  // namespace json_detail

// Parses one JSON value (and nothing after it but spaces).
inline bool json_parse(std::string_view in, JsonValue& out) {
  json_detail::Reader r{in};
  if (!r.value(out)) return false;
  r.ws();
  return r.i == in.size();
}

}  // namespace fplussearch
