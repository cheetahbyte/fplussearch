#include "search.hpp"

#include <cstdlib>
#include <iostream>
#include <string_view>

using namespace fplussearch;

bool matches(std::string_view name, std::string_view query) {
  const Query q = parse_query(query, "/tmp");
  return token_score(name, ~uint64_t(0), q.tokens.at(0)) != kNoMatch;
}

int main() {
  struct Case { std::string_view name, query; bool expected; };
  const Case cases[] = {
      {"loop2.c", "loo2.c", true},
      {"loop2.c", "loopp2.c", true},
      {"loop2.c", "oop2.c", true},
      {"loop2.c", "xloop2.c", true},
      {"loop2.c", "loox2.c", true},
      {"loop2.c", "lopo2.c", true},
      {"prefix loop2.c backup", "loo2.c", true},
      {"prefix loop2.c backup", "loopp2.c", true},
      {"loop2.c", "loop3.c", false},
      {"loop2.c", "loop.c", false},
      {"loop2.c", "loop22.c", false},
      {"loop2.c", "loo2p.c", false},
      {"loop2.c", "loopc.2", false},
      {"loop2.c", "'loo2.c", false},
      {"r_e_a_d_m_e.md", "readme", false},
      {"README.md", "readme", true},
  };
  for (const Case& c : cases) {
    if (matches(c.name, c.query) != c.expected) {
      std::cerr << c.query << " -> " << c.name << ": expected " << c.expected << '\n';
      return EXIT_FAILURE;
    }
  }
  std::cout << "Search matching regression checks passed\n";
}
