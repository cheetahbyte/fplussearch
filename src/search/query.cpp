#include "search/query.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <ctime>

#include "index/index.hpp"
#include "search/detail/scoring.hpp"

namespace fplussearch {

// ---------------------------------------------------------------- parsing

namespace {

uint64_t parse_size(std::string_view s, bool& ok) {
  size_t i = 0;
  double v = 0;
  bool digits = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, digits = true) v = v * 10 + (s[i] - '0');
  if (i < s.size() && s[i] == '.') {
    double scale = 0.1;
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, scale /= 10, digits = true) v += (s[i] - '0') * scale;
  }
  std::string unit;
  for (; i < s.size(); ++i) unit += char(kFold[uint8_t(s[i])]);
  double mult = 0;
  if (unit.empty() || unit == "b") mult = 1;
  else if (unit == "k" || unit == "kb" || unit == "kib") mult = 1024.0;
  else if (unit == "m" || unit == "mb" || unit == "mib") mult = 1024.0 * 1024;
  else if (unit == "g" || unit == "gb" || unit == "gib") mult = 1024.0 * 1024 * 1024;
  else if (unit == "t" || unit == "tb" || unit == "tib") mult = 1024.0 * 1024 * 1024 * 1024;
  ok = digits && mult > 0;
  return uint64_t(v * mult);
}

// An age like 7d, 3h, 2w, 6mo, 1y (bare numbers are days), in seconds.
uint64_t parse_age(std::string_view s, bool& ok) {
  size_t i = 0;
  double v = 0;
  bool digits = false;
  for (; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, digits = true) v = v * 10 + (s[i] - '0');
  if (i < s.size() && s[i] == '.') {
    double scale = 0.1;
    for (++i; i < s.size() && s[i] >= '0' && s[i] <= '9'; ++i, scale /= 10, digits = true) v += (s[i] - '0') * scale;
  }
  const std::string_view u = s.substr(i);
  double mult = 0;
  if (u == "s") mult = 1;
  else if (u == "m" || u == "min") mult = 60;
  else if (u == "h") mult = 3600;
  else if (u.empty() || u == "d") mult = 86400;
  else if (u == "w") mult = 604800;
  else if (u == "mo") mult = 2592000;
  else if (u == "y") mult = 31536000;
  ok = digits && mult > 0;
  return uint64_t(v * mult);
}

// Kinds one type: value allows, or 0 if unknown.
uint32_t type_bits(std::string_view v) {
  if (v == "dir" || v == "folder" || v == "directory") return kTypeDir;
  if (v == "file") return kTypeFiles;
  if (v == "video" || v == "movie") return 1u << CatVideo;
  if (v == "audio" || v == "music") return 1u << CatAudio;
  if (v == "image" || v == "photo" || v == "picture") return 1u << CatImage;
  if (v == "doc" || v == "document") return 1u << CatDoc;
  if (v == "archive" || v == "zip") return 1u << CatArchive;
  if (v == "code" || v == "source") return 1u << CatCode;
  if (v == "font") return 1u << CatFont;
  return 0;
}

std::vector<std::string_view> split(std::string_view s, char sep) {
  std::vector<std::string_view> out;
  for (size_t i = 0; i <= s.size();) {
    size_t j = s.find(sep, i);
    if (j == std::string_view::npos) j = s.size();
    if (j > i) out.push_back(s.substr(i, j - i));
    i = j + 1;
  }
  return out;
}

std::string fold_str(std::string_view s) {
  std::string out(s);
  for (auto& c : out) c = char(kFold[uint8_t(c)]);
  return out;
}

// Words split on spaces; "double quotes" keep spaces inside one word.
std::vector<std::string> split_words(std::string_view s) {
  std::vector<std::string> out;
  std::string cur;
  bool quoted = false;
  for (char c : s) {
    if (c == '"') {
      quoted = !quoted;
    } else if (c == ' ' && !quoted) {
      if (!cur.empty()) out.push_back(std::move(cur)), cur.clear();
    } else {
      cur += c;
    }
  }
  if (!cur.empty()) out.push_back(std::move(cur));
  return out;
}

void push_token(Query& q, std::string_view w) {
  Token t;
  if (w.starts_with('!')) w.remove_prefix(1), t.negate = true, t.mode = Mode::Exact;
  if (w.starts_with('\'')) w.remove_prefix(1), t.mode = Mode::Exact;
  else if (w.starts_with('^')) w.remove_prefix(1), t.mode = Mode::Prefix;
  else if (w.ends_with('$')) w.remove_suffix(1), t.mode = Mode::Suffix;
  if (w.empty()) return;
  size_t positive = 0;
  for (const Token& x : q.tokens) positive += !x.negate;
  if (!t.negate && positive >= 8) return;  // matched tokens are tracked in 8 bits
  t.text = fold_str(w);
  for (char c : t.text) t.mask |= char_bit(uint8_t(c));
  if (detail::takes_typos(t)) {
    t.loose = t.mask & ~char_bit(uint8_t(t.text[0]));
    t.start = start_bit(uint8_t(t.text[0]));
  }
  q.tokens.push_back(std::move(t));
}

// "<x", "<=x", ">x", ">=x", "a..b", "=x" or bare "x" as [lo, hi].
template <typename Parse>
bool parse_range(std::string_view v, Parse&& p, uint64_t& lo, uint64_t& hi, bool bare_is_max) {
  bool ok = false;
  lo = 0, hi = UINT64_MAX;
  if (v.starts_with(">=")) lo = p(v.substr(2), ok);
  else if (v.starts_with('>')) lo = p(v.substr(1), ok) + 1;
  else if (v.starts_with("<=")) hi = p(v.substr(2), ok);
  else if (v.starts_with('<')) {
    const uint64_t x = p(v.substr(1), ok);
    if (x == 0) lo = UINT64_MAX, hi = 0;
    else hi = x - 1;
  } else if (v.starts_with('=')) lo = hi = p(v.substr(1), ok);
  else if (const size_t dots = v.find(".."); dots != std::string_view::npos) {
    bool ok2 = false;
    lo = p(v.substr(0, dots), ok);
    hi = p(v.substr(dots + 2), ok2);
    ok = ok && ok2;
  } else if (bare_is_max) hi = p(v, ok);
  else lo = p(v, ok);
  return ok;
}

}  // namespace

bool Query::has_tokens() const { return !tokens.empty(); }

bool Query::selects() const {
  return !tokens.empty() || !exts.empty() || types != kTypeAny || kind != Kind::Any || !scope.empty() || has_size ||
         min_mtime != 0 || max_mtime != UINT32_MAX || name_re || path_re;
}

Query parse_query(std::string_view text, std::string_view home) {
  Query q;
  auto bad = [&](std::string_view tok, std::string why = "ignored ") {
    if (q.error.empty()) q.error = why + std::string(tok);
  };
  for (const std::string& word : split_words(text)) {
    const size_t colon = word.find(':');
    if (colon != std::string::npos && colon > 0) {
      const std::string key = fold_str(std::string_view(word).substr(0, colon));
      const std::string_view raw = std::string_view(word).substr(colon + 1);
      const std::string low = fold_str(raw);
      const std::string_view v = low;
      bool known = true;
      if (key == "size") {
        uint64_t lo, hi;
        if (!parse_range(v, parse_size, lo, hi, false)) bad(word);
        else {
          q.has_size = true;
          q.min_size = std::max(q.min_size, lo);
          q.max_size = std::min(q.max_size, hi);
        }
      } else if (key == "mtime" || key == "modified") {
        // mtime:<7d is "modified within the last 7 days"; mtime:>1y "longer ago".
        uint64_t lo, hi;
        if (!parse_range(v, parse_age, lo, hi, true)) bad(word);
        else {
          const uint64_t now = uint64_t(std::time(nullptr));
          q.min_mtime = std::max<uint32_t>(q.min_mtime, hi >= now ? 0 : uint32_t(now - hi));
          q.max_mtime = std::min<uint32_t>(q.max_mtime, lo >= now ? 0 : uint32_t(now - lo));
        }
      } else if (key == "type") {
        uint32_t any = 0;
        for (auto p : split(v, ',')) {
          if (p == "app") {
            q.kind = Kind::Dir;
            q.exts.push_back("app");
            any |= kTypeDir;
            continue;
          }
          const uint32_t bits = type_bits(p);
          if (!bits) bad(word, "unknown type in ");
          any |= bits;
        }
        if (any) q.types &= any;
      } else if (key == "kind") {
        if (v == "file" || v == "f") q.kind = Kind::File;
        else if (v == "dir" || v == "folder" || v == "d") q.kind = Kind::Dir;
        else if (v == "link" || v == "symlink" || v == "l") q.kind = Kind::Link;
        else bad(word, "unknown kind in ");
      } else if (key == "ext") {
        for (auto p : split(v, ',')) q.exts.push_back(std::string(p.starts_with('.') ? p.substr(1) : p));
      } else if (key == "in") {
        std::string p(raw);
        if (p.starts_with('~')) p = std::string(home) + p.substr(1);
        struct ScopeCache {
          std::string path, resolved;
          std::chrono::steady_clock::time_point expires;
        };
        static thread_local ScopeCache cache;
        const auto now = std::chrono::steady_clock::now();
        // shortcut: scope symlink changes can lag one second, use event invalidation if instant retargeting is needed.
        if (p.starts_with('/') && p == cache.path && now < cache.expires) {
          p = cache.resolved;
        } else if (char* real = realpath(p.c_str(), nullptr)) {
          if (p.starts_with('/')) cache = {p, real, now + std::chrono::seconds(1)};
          p = real;
          std::free(real);
        }
        while (p.size() > 1 && p.back() == '/') p.pop_back();
        q.scope = p;
      } else if (key == "re" || key == "path") {
        std::string err;
        auto re = Regex::compile(raw, true, false, err);
        if (!re) bad(word, err + " in ");
        (key == "re" ? q.name_re : q.path_re) = re;
      } else if (key == "limit") {
        q.limit = size_t(std::strtoull(low.c_str(), nullptr, 10));
      } else if (key == "sym" || key == "symbol") {
        if (!v.empty()) q.syms.push_back(std::string(v));
      } else if (key == "grep" || key == "content") {
        q.grep = raw, q.grep_mode = GrepMode::Literal;
      } else if (key == "regex") {
        q.grep = raw, q.grep_mode = GrepMode::Regex;
      } else {
        known = false;
      }
      if (known) continue;
    }
    for (auto piece : split(word, '/')) push_token(q, piece);
  }
  return q;
}

}  // namespace fplussearch
