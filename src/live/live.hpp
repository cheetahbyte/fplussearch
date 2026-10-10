#pragma once

#include <sys/stat.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "content/content_index.hpp"
#include "index/index.hpp"
#include "live/watcher.hpp"
#include "search/engine.hpp"
#include "symbols/symbol_index.hpp"

namespace fplussearch {

// The index as it is now: the cached index plus an overlay of what FSEvents
// reported since (entries gone, entries added), folded into a new cache now
// and then. One process on the machine owns the cache and writes it; others
// follow along and reload it when it changes.
class Live {
 public:
  struct Options {
    std::string root;
    std::string cache_file;
    unsigned scan_threads = 8;
    bool content = true;  // keep a content index (grep:) of the text files under content_area
    std::string content_area;  // default: the home folder when root is "/", else root
  };

  // What searches see, unchanging while held.
  struct State {
    std::shared_ptr<const Index> ix;
    std::shared_ptr<const SymbolIndex> sx;
    DeadWords dead;                               // index entries gone since
    size_t dead_count = 0;
    OverlayMap<std::shared_ptr<const Extra>> extras;  // by path
    Overlay overlay;                                          // views dead and extras
    uint64_t event_id = 0;                                    // last event applied

    State() = default;
    State(const State& s)
        : ix(s.ix), sx(s.sx), dead(s.dead), dead_count(s.dead_count), extras(s.extras), event_id(s.event_id) {}
  };

  struct Status {
    size_t entries = 0, dirs = 0, overlay = 0, removed = 0, symbols = 0;
    uint64_t event_id = 0;
    bool owner = false, ready = false, busy = false;
    size_t content_docs = 0, content_bytes = 0;
    bool content_pending = false;
  };

  Live(Options o, Engine& engine);
  ~Live();
  Live(const Live&) = delete;
  Live& operator=(const Live&) = delete;

  // Follows changes in the background, building the index first when `ix`
  // is null or can't be brought up to date. `on_change` runs on a
  // background thread whenever what searches see changed.
  void start(std::shared_ptr<const Index> ix, std::function<void()> on_change = {});
  // Stops following; waits for the background work unless it is a long
  // scan, which is abandoned (the caller is about to exit).
  void stop();

  std::shared_ptr<const State> state() const;
  Results search(const State& s, const Query& q, size_t limit);
  // Paths of the indexed files matching the query's name words and filters
  // (no ranking), at most `max`.
  std::vector<std::string> files(const State& s, const Query& q, size_t max);
  Status status() const;
  // The content index, or null.
  const ContentIndex* content() const { return content_.get(); }
  bool busy() const { return busy_; }
  ScanProgress& progress() { return progress_; }

 private:
  enum class Next { Stop, Rescan, Reload };
  void run(std::shared_ptr<const Index> ix);
  Next follow(Watcher& w);
  void publish(std::shared_ptr<State> s);
  std::shared_ptr<State> fresh_state(std::shared_ptr<const Index> ix) const;
  std::shared_ptr<const SymbolIndex> load_syms(const Index& ix) const;
  std::shared_ptr<const Index> build_full();
  void apply_dir(State& s, const std::string& dir, bool tree, int depth = 0);
  void start_compaction(const State& s);
  bool cache_changed() const;
  void content_loop();
  void content_sync(std::shared_ptr<const Index> ix);
  void content_update(const Changes& c);

  Options opt_;
  Engine& engine_;
  std::function<void()> on_change_;
  mutable std::mutex state_m_;
  std::shared_ptr<const State> state_;
  std::thread thread_;
  std::atomic<bool> stop_{false};
  std::atomic<bool> busy_{false};
  std::atomic<bool> scanning_{false};  // a full scan: not worth waiting for on stop
  ScanProgress progress_;
  int lock_fd_ = -1;
  bool owner_ = false;
  std::vector<std::string> ignore_;

  // Compaction: a refreshed index built on its own thread from the folders
  // touched since the current one, handed back to run().
  std::set<std::string> touched_, touched_trees_;   // since the index was built
  std::set<std::string> after_, after_trees_;       // since the running compaction's snapshot
  std::thread compactor_;
  std::atomic<bool> compacting_{false};
  std::mutex compacted_m_;
  std::shared_ptr<const Index> compacted_;
  std::chrono::steady_clock::time_point last_compact_ = std::chrono::steady_clock::now();
  // Content index upkeep runs on its own thread: syncing after a new index,
  // relisting folders as they change.
  std::unique_ptr<ContentIndex> content_;
  std::thread content_thread_;
  std::mutex content_m_;
  std::condition_variable content_cv_;
  std::shared_ptr<const Index> content_ix_;  // sync against this
  std::vector<std::string> content_dirs_, content_trees_;
  std::atomic<bool> content_busy_{false};
  struct stat cache_stat_ {};  // of the cache file we last loaded or wrote
  std::mutex search_m_;
};

}  // namespace fplussearch
