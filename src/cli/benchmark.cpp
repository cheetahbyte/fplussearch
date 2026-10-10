#include "cli/benchmark.hpp"
#include "cli/runtime.hpp"
#include "symbols/symbol_index.hpp"

#include <mach/mach.h>

#include <algorithm>
#include <cstdio>
#include <thread>

namespace fplussearch::cli {

namespace {

// Physical footprint excludes clean pages of the mapped index, which the OS
// can drop and re-read at any time; resident size includes them.
void print_memory() {
  task_vm_info_data_t info{};
  mach_msg_type_number_t n = TASK_VM_INFO_COUNT;
  if (task_info(mach_task_self(), TASK_VM_INFO, task_info_t(&info), &n) != KERN_SUCCESS) return;
  std::printf("process: %.1f MB footprint, %.1f MB resident\n", info.phys_footprint / 1e6,
              info.resident_size / 1e6);
}

}  // namespace

Results run_query(const Index& ix, const SymbolIndex* sx, Engine& engine, const Query& q, size_t limit) {
  if (q.syms.empty()) return engine.search(ix, q, limit);
  return sx ? engine.search_symbols(ix, *sx, q, limit) : Results{};
}

int bench(const Index& ix, const SymbolIndex* sx, Engine& engine, std::vector<std::string> queries) {
  if (queries.empty()) {
    queries = {"size:>1gb type:video", "a", "e", "readme", "config.json", "ext:mp4", "type:image",
               "lib", "node_modules", "zzqxj", "png ico", "size:>100mb"};
    if (sx) queries.insert(queries.end(), {"sym:parse", "sym:render ext:tsx", "sym:main type:file", "sym:x"});
  }
  std::printf("%zu entries (%u directories), index %.1f MB\n", ix.count(), ix.dirs, ix.bytes / 1e6);
  if (sx) std::printf("%zu symbol definitions, symbol index %.1f MB\n", sx->occ.n, sx->bytes / 1e6);
  // hot: back to back. typing: 80 ms apart, like keystrokes. cold: 500 ms
  // apart, after the workers have gone to sleep and the cores have idled.
  std::printf("%-24s %10s %10s %10s %10s\n", "query", "matches", "hot ms", "typing ms", "cold ms");
  auto timed = [&](const std::string& qs, Results& r) {
    auto t0 = Clock::now();
    Query q = parse_query(qs, home());
    r = run_query(ix, sx, engine, q, 50);
    return ms_since(t0);
  };
  auto median = [](std::vector<double> v) {
    std::sort(v.begin(), v.end());
    return v[v.size() / 2];
  };
  for (const auto& qs : queries) {
    std::vector<double> hot, typing, cold;
    Results r;
    for (int i = 0; i < 40; ++i) hot.push_back(timed(qs, r));
    for (int i = 0; i < 11; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(80));
      typing.push_back(timed(qs, r));
    }
    for (int i = 0; i < 5; ++i) {
      std::this_thread::sleep_for(std::chrono::milliseconds(500));
      cold.push_back(timed(qs, r));
    }
    std::printf("%-24s %10llu %10.3f %10.3f %10.3f\n", qs.c_str(), (unsigned long long)r.total,
                median(hot), median(typing), median(cold));
  }
  print_memory();
  return 0;
}

}  // namespace fplussearch::cli
