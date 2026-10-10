#include "index/index.hpp"

#include <memory>

#include "index/cache_format.hpp"
#include "index/scanner.hpp"
#include "live/watcher.hpp"
#include "storage/fields.hpp"
#include "symbols/symbols.hpp"
#include "symbols/symbol_index.hpp"

namespace fplussearch {

using namespace index_detail;

namespace {

Index save_and_index_symbols(std::unique_ptr<Built> built, const std::string& cache_file, ScanProgress* progress,
                             bool background, const std::function<void(const Index&)>& on_names = {}) {
  Index ix;
  if (!save_fields(cache_file, kMagic, built->meta, built->fields) || !load_index(ix, cache_file))
    attach(ix, hold_fields(built->meta, std::move(built->fields)));  // could not write the cache
  built.reset();
  if (on_names) on_names(ix);
  build_symbols(ix, symbol_file(cache_file), progress, background);
  return ix;
}

}  // namespace

Index build_index(const std::string& root, unsigned threads, ScanProgress* progress, bool background,
                  const std::string& cache_file, const std::function<void(const Index&)>& on_names) {
  std::string r = root;
  while (r.size() > 1 && r.back() == '/') r.pop_back();
  // Taken before the scan: replaying from here repeats changes the scan may
  // already have seen, which is harmless, and misses none.
  const uint64_t event_id = current_event_id();
  auto built = std::make_unique<Built>(Scanner(r, progress).run(threads ? threads : 1, background));
  built->meta[kMetaEventId] = event_id;
  return save_and_index_symbols(std::move(built), cache_file, progress, background, on_names);
}

Index refresh_index(const Index& old, const Changes& changes, unsigned threads, ScanProgress* progress,
                    bool background, const std::string& cache_file) {
  auto built =
      std::make_unique<Built>(Scanner(old.root, progress, &old, &changes).run(threads ? threads : 1, background));
  built->meta[kMetaEventId] = changes.event_id;
  return save_and_index_symbols(std::move(built), cache_file, progress, background);
}

}  // namespace fplussearch
