#include "live/live.hpp"

namespace fplussearch {

namespace {

// Compaction folds the overlay into a new cache once it holds this much.
constexpr size_t kMaxExtras = 50000;
constexpr size_t kMaxDead = 200000;
// ...or this long after the last one, if anything changed.
constexpr auto kCompactEvery = std::chrono::minutes(10);
// Changes reach searches this soon: FSEvents batches events this long.
constexpr double kLatency = 0.05;

}  // namespace

std::shared_ptr<const Index> Live::build_full() {
  busy_ = scanning_ = true;
  progress_.entries = 0;
  progress_.source_files = 0;
  const bool background = state() != nullptr;  // searches already have an index
  // Names are searchable as soon as they're saved; symbols follow.
  auto ix = std::make_shared<const Index>(
      build_index(opt_.root, opt_.scan_threads, &progress_, background, opt_.cache_file, [&](const Index& names) {
        auto s = std::make_shared<State>();
        s->ix = std::make_shared<const Index>(names);
        s->dead.assign((names.count() + 63) / 64, 0);
        s->event_id = names.event_id;
        publish(s);
      }));
  scanning_ = busy_ = false;
  stat(opt_.cache_file.c_str(), &cache_stat_);
  return ix;
}

bool Live::cache_changed() const {
  struct stat st {};
  if (stat(opt_.cache_file.c_str(), &st) != 0) return false;
  const struct stat& old = cache_stat_;
  return st.st_ino != old.st_ino || st.st_mtimespec.tv_sec != old.st_mtimespec.tv_sec ||
         st.st_mtimespec.tv_nsec != old.st_mtimespec.tv_nsec;
}

void Live::run(std::shared_ptr<const Index> ix) {
  if (ix) stat(opt_.cache_file.c_str(), &cache_stat_);
  bool bloom_refresh_scheduled = false;
  while (!stop_) {
    if (!ix || ix->event_id == 0 || ix->event_id > current_event_id()) {
      if (owner_) {
        ix = build_full();
      } else {
        // The owner is scanning: wait for its cache.
        ix = nullptr;
        while (!stop_ && !ix) {
          auto loaded = std::make_shared<Index>();
          if (load_index(*loaded, opt_.cache_file) && loaded->root == opt_.root && loaded->event_id != 0) ix = loaded;
          else std::this_thread::sleep_for(std::chrono::milliseconds(500));
        }
        if (!ix) return;
        stat(opt_.cache_file.c_str(), &cache_stat_);
      }
      if (stop_) return;
      touched_.clear();
      touched_trees_.clear();
      publish(fresh_state(ix));
      content_sync(ix);
    }
    // The content index may be behind the cached index (a first run, or
    // files changed while nothing was running): catch it up.
    if (owner_ && content_) {
      const bool legacy = !bloom_refresh_scheduled && content_->needs_bloom_refresh();
      if (content_->docs() == 0 || legacy) content_sync(ix);
      bloom_refresh_scheduled = bloom_refresh_scheduled || legacy;
    }
    Watcher watcher(opt_.root, ix->event_id, ignore_, kLatency);
    switch (follow(watcher)) {
      case Next::Stop:
        return;
      case Next::Rescan:
        ix = nullptr;
        break;
      case Next::Reload: {
        auto loaded = std::make_shared<Index>();
        ix = load_index(*loaded, opt_.cache_file) && loaded->root == opt_.root ? loaded : nullptr;
        stat(opt_.cache_file.c_str(), &cache_stat_);
        if (ix) publish(fresh_state(ix));
        if (content_) content_->open();  // the owner rewrote it too
        break;
      }
    }
  }
}

Live::Next Live::follow(Watcher& w) {
  auto last_check = std::chrono::steady_clock::now();
  for (;;) {
    if (stop_) return Next::Stop;
    const Watcher::Ready r = w.wait(std::chrono::milliseconds(100), std::chrono::milliseconds(0),
                                    std::chrono::milliseconds(0));
    if (r == Watcher::Ready::Rescan) return Next::Rescan;

    // A finished compaction: its index replaces the current one, and what
    // changed while it ran is applied again on top.
    std::shared_ptr<const Index> compacted;
    {
      std::lock_guard lk(compacted_m_);
      compacted = std::move(compacted_);
    }
    if (compacted) {
      compactor_.join();
      auto s = fresh_state(compacted);
      s->event_id = std::max(s->event_id, state()->event_id);
      touched_ = std::move(after_);
      touched_trees_ = std::move(after_trees_);
      after_.clear();
      after_trees_.clear();
      for (const auto& d : touched_trees_) apply_dir(*s, d, true);
      for (const auto& d : touched_) apply_dir(*s, d, false);
      stat(opt_.cache_file.c_str(), &cache_stat_);
      last_compact_ = std::chrono::steady_clock::now();
      busy_ = false;
      publish(s);
      content_sync(compacted);
    }

    if (!owner_ && std::chrono::steady_clock::now() - last_check > std::chrono::seconds(2)) {
      last_check = std::chrono::steady_clock::now();
      if (cache_changed()) return Next::Reload;
    }

    if (r == Watcher::Ready::Changes) {
      const Changes c = w.take();
      auto s = std::make_shared<State>(*state());
      for (const auto& d : c.trees) {
        apply_dir(*s, d, true);
        touched_trees_.insert(d);
        if (compacting_) after_trees_.insert(d);
      }
      for (const auto& d : c.dirs) {
        apply_dir(*s, d, false);
        touched_.insert(d);
        if (compacting_) after_.insert(d);
      }
      s->event_id = c.event_id;
      publish(s);
      content_update(c);
    }

    const auto s = state();
    if (owner_ && !compacting_ && !touched_.empty() &&
        (s->extras.size() > kMaxExtras || s->dead_count > kMaxDead ||
         std::chrono::steady_clock::now() - last_compact_ > kCompactEvery))
      start_compaction(*s);
  }
}

void Live::start_compaction(const State& s) {
  Changes c;
  c.dirs.assign(touched_.begin(), touched_.end());
  c.trees.assign(touched_trees_.begin(), touched_trees_.end());
  c.event_id = s.event_id;
  compacting_ = true;
  busy_ = true;
  auto base = s.ix;
  compactor_ = std::thread([this, base, c] {
    auto ix = std::make_shared<const Index>(refresh_index(*base, c, opt_.scan_threads, nullptr, true, opt_.cache_file));
    std::lock_guard lk(compacted_m_);
    compacted_ = std::move(ix);
    compacting_ = false;
  });
}

}  // namespace fplussearch
