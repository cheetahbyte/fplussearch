#include "regex.hpp"

#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>

namespace fplussearch {

namespace {

// One match block per thread, big enough for the whole match.
pcre2_match_data* match_data() {
  struct Holder {
    pcre2_match_data* md = pcre2_match_data_create(1, nullptr);
    ~Holder() { pcre2_match_data_free(md); }
  };
  thread_local Holder h;
  return h.md;
}

}  // namespace

std::shared_ptr<const Regex> Regex::compile(std::string_view pattern, bool caseless, bool multiline,
                                            std::string& error) {
  int code = 0;
  PCRE2_SIZE offset = 0;
  uint32_t options = PCRE2_UTF | PCRE2_MATCH_INVALID_UTF;
  if (caseless) options |= PCRE2_CASELESS;
  if (multiline) options |= PCRE2_MULTILINE;
  pcre2_code* re = pcre2_compile(reinterpret_cast<PCRE2_SPTR>(pattern.data()), pattern.size(), options, &code,
                                 &offset, nullptr);
  if (!re) {
    PCRE2_UCHAR buf[256];
    pcre2_get_error_message(code, buf, sizeof buf);
    error = "bad regex: " + std::string(reinterpret_cast<char*>(buf));
    return nullptr;
  }
  pcre2_jit_compile(re, PCRE2_JIT_COMPLETE);  // falls back to the interpreter if JIT is unavailable
  std::shared_ptr<Regex> r(new Regex());
  r->code_ = re;
  r->pattern_ = std::string(pattern);
  return r;
}

Regex::~Regex() { pcre2_code_free(static_cast<pcre2_code*>(code_)); }

bool Regex::matches(std::string_view s) const {
  size_t a, b;
  return find(s, 0, a, b);
}

bool Regex::find(std::string_view s, size_t from, size_t& start, size_t& end) const {
  pcre2_match_data* md = match_data();
  const int rc = pcre2_match(static_cast<pcre2_code*>(code_), reinterpret_cast<PCRE2_SPTR>(s.data()), s.size(), from,
                             0, md, nullptr);
  if (rc < 0) return false;
  const PCRE2_SIZE* ov = pcre2_get_ovector_pointer(md);
  start = ov[0];
  end = ov[1];
  return true;
}

}  // namespace fplussearch
