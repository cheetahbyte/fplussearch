#include "index.hpp"
#include "search.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

int main(int argc, char** argv) {
  if (argc != 5) {
    std::cerr << "Usage: replay INDEX QUERIES THREADS SECONDS\n";
    return 2;
  }
  fplussearch::Index index;
  if (!fplussearch::load_index(index, argv[1])) {
    std::cerr << "Cannot load index\n";
    return 3;
  }
  std::ifstream input(argv[2]);
  if (!input) return 4;
  std::vector<fplussearch::Query> queries;
  for (std::string line; std::getline(input, line);) {
    auto query = fplussearch::parse_query("in:\"" + index.root + "\" " + line);
    if (!query.error.empty()) {
      std::cerr << query.error << '\n';
      return 5;
    }
    queries.push_back(std::move(query));
  }
  if (queries.empty()) return 6;
  fplussearch::Engine engine(unsigned(std::stoul(argv[3])));
  for (size_t i = 0; i < std::min<size_t>(20, queries.size()); ++i)
    engine.search(index, queries[i], 50);
  std::cout << "ready entries=" << index.n << " queries=" << queries.size() << std::endl;
  const auto start = std::chrono::steady_clock::now();
  const auto deadline = start + std::chrono::seconds(std::stoul(argv[4]));
  size_t completed = 0;
  uint64_t total = 0;
  while (std::chrono::steady_clock::now() < deadline) {
    const auto result = engine.search(index, queries[completed % queries.size()], 50);
    total += result.total;
    ++completed;
  }
  std::cout << "completed=" << completed << " total=" << total << '\n';
}
