#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "content/query_plan.hpp"

namespace fplussearch::content_detail {

// What a regex fragment says about matching text: a small set of exact
// (folded) strings it can match, or failing that, a trigram query.
struct Info {
  std::optional<std::vector<std::string>> exact;
  TQ q;
};

// A conservative reader of PCRE syntax: anything it doesn't understand
// becomes "no requirement", which only costs extra candidate reads.
class RegexPlanner {
 public:
  explicit RegexPlanner(std::string_view p, bool caseless = false);
  TQ plan();

 private:
  Info alternation();
  Info sequence();
  Info repeat();
  Info atom();
  Info escape();
  Info char_class();

  std::string_view p_;
  size_t i_ = 0;
  bool caseless_ = false;
  bool safe_ = true;
};

}  // namespace fplussearch::content_detail
