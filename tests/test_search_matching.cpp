#include "search.hpp"

#include <cstdlib>
#include <chrono>
#include <filesystem>
#include <thread>
#include <unistd.h>
#include <cassert>
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
  char temporary[] = "/tmp/fplus-scope-XXXXXX";
  assert(mkdtemp(temporary));
  const std::filesystem::path root = std::filesystem::canonical(temporary);
  const auto a = root / "a", b = root / "b", link = root / "link", missing = root / "missing";
  std::filesystem::create_directory(a);
  std::filesystem::create_directory(b);
  std::filesystem::create_directory_symlink(a, link);
  const std::string scoped = "in:" + link.string() + " readme";
  assert(parse_query(scoped).scope == a.string());
  std::filesystem::remove(link);
  std::filesystem::create_directory_symlink(b, link);
  assert(parse_query(scoped).scope == a.string());
  std::this_thread::sleep_for(std::chrono::milliseconds(1050));
  assert(parse_query(scoped).scope == b.string());
  assert(parse_query("in:" + missing.string()).scope == missing.string());
  std::filesystem::create_directory_symlink(a, missing);
  assert(parse_query("in:" + missing.string()).scope == a.string());
  std::filesystem::remove_all(root);
  std::cout << "Search matching regression checks passed\n";
}
