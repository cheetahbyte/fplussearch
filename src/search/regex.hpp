#pragma once

#include <memory>
#include <string>
#include <string_view>

namespace fplussearch {

// A compiled PCRE2 pattern (JIT when available). Thread-safe to match from
// any number of threads.
class Regex {
 public:
  // Returns null and sets `error` if the pattern does not compile.
  static std::shared_ptr<const Regex> compile(std::string_view pattern, bool caseless, bool multiline,
                                              std::string& error);
  ~Regex();
  Regex(const Regex&) = delete;
  Regex& operator=(const Regex&) = delete;

  bool matches(std::string_view s) const;
  // First match at or after `from`: sets [start, end) and returns true.
  bool find(std::string_view s, size_t from, size_t& start, size_t& end) const;
  const std::string& pattern() const { return pattern_; }

 private:
  Regex() = default;
  void* code_ = nullptr;
  std::string pattern_;
};

}  // namespace fplussearch
