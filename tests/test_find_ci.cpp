#include "../src/search.cpp"

#include <cstdlib>
#include <iostream>
#include <random>
#include <sys/mman.h>
#include <unistd.h>

using namespace fplussearch;

size_t naive_find(std::string_view name, std::string_view query) {
  if (query.size() > name.size()) return std::string_view::npos;
  for (size_t i = 0; i <= name.size() - query.size(); ++i) {
    bool match = true;
    for (size_t j = 0; j < query.size(); ++j)
      if (kFold[uint8_t(name[i + j])] != uint8_t(query[j])) {
        match = false;
        break;
      }
    if (match) return i;
  }
  return std::string_view::npos;
}

int main() {
  const size_t page = size_t(sysconf(_SC_PAGESIZE));
  void* mapping = mmap(nullptr, page * 3, PROT_NONE, MAP_PRIVATE | MAP_ANON, -1, 0);
  if (mapping == MAP_FAILED) return EXIT_FAILURE;
  char* memory = static_cast<char*>(mapping) + page;
  if (mprotect(memory, page, PROT_READ | PROT_WRITE)) {
    munmap(mapping, page * 3);
    return EXIT_FAILURE;
  }
  std::mt19937_64 random(9147);
  size_t checks = 0;
  auto check = [&](std::string_view name, std::string_view query) {
    const size_t expected = naive_find(name, query);
    for (bool end : {false, true}) {
      char* data = end ? memory + page - name.size() : memory;
      std::memcpy(data, name.data(), name.size());
      ++checks;
      if (find_ci(std::string_view(data, name.size()), query) != expected) return false;
    }
    return true;
  };
  for (size_t positions = 1; positions <= 65; ++positions) {
    for (size_t k : {size_t(1), size_t(2), size_t(3), size_t(16), size_t(17)}) {
      for (size_t at = 0; at < positions; ++at) {
        std::string name(positions + k - 1, 'x'), query(k, 'a');
        name.replace(at, k, std::string(k, 'A'));
        if (!check(name, query)) {
          std::cerr << "Boundary mismatch: " << positions << ' ' << k << ' ' << at << '\n';
          munmap(mapping, page * 3);
          return EXIT_FAILURE;
        }
      }
    }
  }
  for (size_t trial = 0; trial < 200000; ++trial) {
    const size_t n = trial % 513;
    const size_t k = trial % 7 == 0 ? n : trial % 7 == 1 ? n + 1 : trial % 7 == 2 ? 1 : random() % 80;
    std::string name(n, 'a'), query(k, 'a');
    for (char& c : name) c = trial % 3 == 0 ? char(random()) : "aaA__.-001bBZ"[random() % 13];
    for (char& c : query) c = char(kFold[uint8_t(random())]);
    if (k <= n && trial % 2) {
      const size_t at = trial % 5 == 0 ? n - k : random() % (n - k + 1);
      for (size_t j = 0; j < k; ++j) query[j] = char(kFold[uint8_t(name[at + j])]);
    }
    if (!check(name, query)) {
      std::cerr << "Randomized mismatch: " << trial << '\n';
      munmap(mapping, page * 3);
      return EXIT_FAILURE;
    }
  }
  munmap(mapping, page * 3);
  std::cout << checks << " substring differential checks passed, including guard-page boundaries\n";
}
