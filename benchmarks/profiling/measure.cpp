#include "index.hpp"
#include "search.hpp"

#include <algorithm>
#include <chrono>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <string>
#include <sys/resource.h>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
struct Item {
  std::string group;
  fplussearch::Query query;
};

double cpu_seconds(const rusage& usage) {
  return usage.ru_utime.tv_sec + usage.ru_stime.tv_sec +
         (usage.ru_utime.tv_usec + usage.ru_stime.tv_usec) / 1e6;
}
}

int main(int argc, char** argv) {
  if (argc != 6 && argc != 7) {
    std::cerr << "Usage: measure INDEX WORKLOAD ROUNDS LIMIT THREADS [RESPONSES]\n";
    return 2;
  }
  const unsigned rounds = unsigned(std::stoul(argv[3]));
  if (rounds < 1 || rounds > 3) return 2;
  const size_t limit = std::stoull(argv[4]);
  const unsigned threads = unsigned(std::stoul(argv[5]));
  if (threads == 0) return 2;
  fplussearch::Index index;
  const auto load_start = Clock::now();
  if (!fplussearch::load_index(index, argv[1])) return 3;
  const double load_ms = std::chrono::duration<double, std::milli>(Clock::now() - load_start).count();
  std::ifstream input(argv[2]);
  if (!input) return 4;
  std::vector<Item> items;
  for (std::string line; std::getline(input, line);) {
    const size_t tab = line.find('\t');
    if (tab == std::string::npos || tab == 0) return 5;
    const std::string group = line.substr(0, tab);
    if (group == "all" || !std::all_of(group.begin(), group.end(), [](unsigned char c) {
          return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                 (c >= '0' && c <= '9') || c == '_' || c == '-';
        })) return 5;
    auto query = fplussearch::parse_query(line.substr(tab + 1));
    if (!query.error.empty()) return 6;
    items.push_back({group, std::move(query)});
  }
  if (items.empty()) return 7;
  fplussearch::Engine engine(threads);
  for (size_t i = 0; i < std::min<size_t>(20, items.size()); ++i)
    engine.search(index, items[i].query, limit);
  std::ofstream responses;
  if (argc == 7) {
    responses.open(argv[6]);
    if (!responses) return 8;
  }
  std::map<std::string, std::vector<double>> times;
  uint64_t digest = 14695981039346656037ULL;
  auto add = [&](uint64_t value) { digest = (digest ^ value) * 1099511628211ULL; };
  rusage before{}, after{};
  getrusage(RUSAGE_SELF, &before);
  const auto start = Clock::now();
  for (unsigned round = 0; round < rounds; ++round) {
    for (size_t n = 0; n < items.size(); ++n) {
      const auto t0 = Clock::now();
      const auto result = engine.search(index, items[n].query, limit);
      const double us = std::chrono::duration<double, std::micro>(Clock::now() - t0).count();
      times[items[n].group].push_back(us);
      times["all"].push_back(us);
      add(result.total);
      for (size_t i = 0; i < result.top.size(); ++i) {
        add(result.top[i]);
        add(uint64_t(result.score[i]));
      }
      if (responses.is_open() && round == 0) {
        responses << n << '\t' << result.total;
        for (size_t i = 0; i < result.top.size(); ++i)
          responses << '\t' << result.top[i] << ':' << result.score[i];
        responses << '\n';
      }
    }
  }
  const double wall_s = std::chrono::duration<double>(Clock::now() - start).count();
  getrusage(RUSAGE_SELF, &after);
  std::cout << std::setprecision(10) << "{\"threads\":" << threads << ",\"rounds\":" << rounds
            << ",\"limit\":" << limit << ",\"index_bytes\":" << index.bytes
            << ",\"entries\":" << index.n << ",\"dirs\":" << index.dirs
            << ",\"load_ms\":" << load_ms << ",\"peak_rss_bytes\":" << after.ru_maxrss
            << ",\"cpu_s\":" << cpu_seconds(after) - cpu_seconds(before)
            << ",\"wall_s\":" << wall_s << ",\"digest\":\"" << digest << "\",\"groups\":{";
  bool first = true;
  for (auto& [group, values] : times) {
    std::sort(values.begin(), values.end());
    const size_t n = values.size();
    if (!first) std::cout << ',';
    first = false;
    const double median = (values[(n - 1) / 2] + values[n / 2]) / 2;
    std::cout << std::quoted(group) << ":{\"n\":" << n << ",\"p50_us\":" << median
              << ",\"p90_us\":" << values[std::min(n - 1, n * 9 / 10)] << '}';
  }
  std::cout << "}}\n";
}
