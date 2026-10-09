// Live: the cached index plus an overlay of changes FSEvents reported since,
// applied one folder at a time and folded into a new cache now and then.

#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>

#include <unordered_map>

#include "live.hpp"

namespace fplussearch {

namespace {

// Compaction folds the overlay into a new cache once it holds this much.
constexpr size_t kMaxExtras = 50000;
constexpr size_t kMaxDead = 200000;
// ...or this long after the last one, if anything changed.
constexpr auto kCompactEvery = std::chrono::minutes(10);
// Changes reach searches this soon: FSEvents batches events this long.
constexpr double kLatency = 0.05;

std::string join(const std::string& dir, std::string_view name) {
  std::string s = dir;
  if (dir != "/") s += '/';
  s += name;
  return s;
}

std::string parent_of(const std::string& p) {
  const size_t slash = p.rfind('/');
  return slash == 0 || slash == std::string::npos ? "/" : p.substr(0, slash);
}

bool under(std::string_view path, std::string_view dir) {
  return dir == "/" || path == dir || (path.starts_with(dir) && path.size() > dir.size() && path[dir.size()] == '/');
}

std::shared_ptr<const Extra> make_extra(const std::string& path, const Listed& l) {
  auto x = std::make_shared<Extra>();
  x->path = path;
  x->dir = l.dir;
  x->kind = l.kind;
  x->size = l.size;
  x->mtime = uint32_t(std::min<uint64_t>(l.mtime / 1000000000, UINT32_MAX));
  x->flags = l.flags;
  x->mask = name_mask(std::string_view(path).substr(path.rfind('/') + 1));
  return x;
}

bool dead_at(const Live::State& s, uint32_t e) { return (s.dead[e >> 6] >> (e & 63)) & 1; }

void kill(Live::State& s, uint32_t e) {
  uint64_t& w = s.dead[e >> 6];
  const uint64_t bit = uint64_t(1) << (e & 63);
  if (!(w & bit)) w |= bit, ++s.dead_count;
}

void kill_subtree(Live::State& s, uint32_t e) {
  std::vector<uint32_t> stack{e};
  while (!stack.empty()) {
    const uint32_t c = stack.back();
    stack.pop_back();
    kill(s, c);
    if (s.ix->is_dir(c))
      for (uint32_t k : s.ix->kids(c)) stack.push_back(k);
  }
}

// Drops `path` and everything under it from the overlay.
void drop_extras(Live::State& s, const std::string& path) {
  s.extras.erase(path);
  const std::string lo = path == "/" ? "/" : path + "/";
  const std::string hi = path == "/" ? "0" : path + "0";  // '0' follows '/'
  s.extras.erase_range(lo, hi);
}

}  // namespace

Live::Live(Options o, Engine& engine) : opt_(std::move(o)), engine_(engine) {
  ignore_ = excluded_dirs(opt_.root);
  ignore_.push_back(opt_.cache_file.substr(0, opt_.cache_file.rfind('/')));  // our own writes
  if (opt_.content) {
    std::string area = opt_.content_area;
    if (area.empty()) area = opt_.root != "/" ? opt_.root : std::getenv("HOME") ? std::getenv("HOME") : "";
    if (char* real = realpath(area.c_str(), nullptr)) {
      area = real;
      std::free(real);
    }
    const std::string dir = opt_.cache_file.substr(0, opt_.cache_file.rfind(".bin")) + ".content";
    if (!area.empty()) content_ = std::make_unique<ContentIndex>(dir, area);
  }
}

Live::~Live() {
  stop();
  if (lock_fd_ >= 0) close(lock_fd_);
}

void Live::start(std::shared_ptr<const Index> ix, std::function<void()> on_change) {
  lock_fd_ = open((opt_.cache_file + ".lock").c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0644);
  owner_ = lock_fd_ >= 0 && flock(lock_fd_, LOCK_EX | LOCK_NB) == 0;
  on_change_ = std::move(on_change);
  if (content_) {
    content_->open();
    if (owner_) content_thread_ = std::thread([this] { content_loop(); });
  }
  if (ix) publish(fresh_state(ix));
  thread_ = std::thread([this, ix] { run(ix); });
}

void Live::content_sync(std::shared_ptr<const Index> ix) {
  if (!content_ || !owner_) return;
  std::lock_guard lk(content_m_);
  content_ix_ = std::move(ix);
  content_cv_.notify_one();
}

void Live::content_update(const Changes& c) {
  if (!content_ || !owner_) return;
  std::lock_guard lk(content_m_);
  content_dirs_.insert(content_dirs_.end(), c.dirs.begin(), c.dirs.end());
  content_trees_.insert(content_trees_.end(), c.trees.begin(), c.trees.end());
  content_cv_.notify_one();
}

void Live::content_loop() {
  for (;;) {
    std::shared_ptr<const Index> ix;
    std::vector<std::string> dirs, trees;
    {
      std::unique_lock lk(content_m_);
      content_cv_.wait(lk, [&] { return stop_ || content_ix_ || !content_dirs_.empty() || !content_trees_.empty(); });
      if (stop_) return;
      ix = std::move(content_ix_);
      dirs.swap(content_dirs_);
      trees.swap(content_trees_);
    }
    content_busy_ = true;
    if (ix) content_->sync(*ix, true);
    if (!dirs.empty() || !trees.empty()) content_->update_paths(dirs, trees);
    content_busy_ = false;
  }
}

void Live::stop() {
  stop_ = true;
  if (thread_.joinable()) {
    if (scanning_) thread_.detach();
    else thread_.join();
  }
  // A compaction finishing writes a cache file the next start can use; one
  // cut short only leaves a temporary file behind.
  if (compactor_.joinable()) compactor_.detach();
  if (content_thread_.joinable()) {
    {
      std::lock_guard lk(content_m_);
      content_cv_.notify_all();
    }
    if (content_busy_) content_thread_.detach();  // a sync can take a while; its segments are written atomically
    else content_thread_.join();
  }
}

std::shared_ptr<const Live::State> Live::state() const {
  std::lock_guard lk(state_m_);
  return state_;
}

Results Live::search(const State& s, const Query& q, size_t limit) {
  std::lock_guard lk(search_m_);  // the engine's pool runs one search at a time
  if (!q.syms.empty()) return s.sx ? engine_.search_symbols(*s.ix, *s.sx, q, limit) : Results{};
  return engine_.search(*s.ix, q, limit, &s.overlay);
}

std::vector<std::string> Live::files(const State& s, const Query& q, size_t max) {
  std::vector<uint32_t> hits;
  {
    std::lock_guard lk(search_m_);
    std::mutex m;
    engine_.each_file(*s.ix, q, [&](uint32_t e) {
      if ((s.dead[e >> 6] >> (e & 63)) & 1) return;
      std::lock_guard g(m);
      if (hits.size() < max) hits.push_back(e);
    });
  }
  std::vector<std::string> out;
  out.reserve(hits.size());
  for (uint32_t e : hits) out.push_back(s.ix->path(e));
  for (const auto& [path, x] : s.extras)
    if (out.size() < max && !x->dir && (q.scope.empty() || path.starts_with(q.scope + "/")) && path_matches(q, path))
      out.push_back(path);
  std::sort(out.begin(), out.end());
  return out;
}

Live::Status Live::status() const {
  Status st;
  const auto s = state();
  st.owner = owner_;
  st.busy = busy_;
  if (!s) return st;
  st.ready = true;
  st.entries = s->ix->count() - s->dead_count + s->extras.size();
  st.dirs = s->ix->dirs;
  st.overlay = s->extras.size();
  st.removed = s->dead_count;
  st.symbols = s->sx ? s->sx->occ.n : 0;
  st.event_id = s->event_id;
  if (content_) {
    st.content_docs = content_->docs();
    st.content_bytes = content_->bytes();
    st.content_pending = content_busy_;
  }
  return st;
}

std::shared_ptr<const SymbolIndex> Live::load_syms(const Index& ix) const {
  auto sx = std::make_shared<SymbolIndex>();
  return load_symbols(*sx, symbol_file(opt_.cache_file), ix) ? sx : nullptr;
}

std::shared_ptr<Live::State> Live::fresh_state(std::shared_ptr<const Index> ix) const {
  auto s = std::make_shared<State>();
  s->sx = load_syms(*ix);
  s->dead.assign((ix->count() + 63) / 64, 0);
  s->event_id = ix->event_id;
  s->ix = std::move(ix);
  return s;
}

void Live::publish(std::shared_ptr<State> s) {
  s->overlay.dead = &s->dead;
  s->overlay.extras.clear();
  s->overlay.extras.reserve(s->extras.size());
  for (const auto& [path, x] : s->extras) s->overlay.extras.push_back(x.get());
  {
    std::lock_guard lk(state_m_);
    state_ = std::move(s);
  }
  if (on_change_) on_change_();
}

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
    if (owner_ && content_ && content_->docs() == 0) content_sync(ix);
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

// Brings folder `dir` in line with the disk: lists it and diffs the listing
// against the index and the overlay. With `tree`, its whole subtree is
// listed again instead. Idempotent, so replays and duplicates are harmless.
void Live::apply_dir(State& s, const std::string& dir, bool tree, int depth) {
  for (const auto& ig : ignore_)
    if (under(dir, ig)) return;
  const Index& ix = *s.ix;
  const uint32_t e = ix.lookup(dir);
  const bool in_index = e != UINT32_MAX && ix.is_dir(e) && !dead_at(s, e);
  const auto ox = s.extras.find(dir);
  const bool in_overlay = ox != s.extras.end() && ox->second->dir;

  auto add = [&](const std::string& path, const Listed& l) {
    s.extras.set(path, make_extra(path, l));
    if (!l.dir) return;
    // A new folder: everything under it is new too.
    std::vector<std::string> stack{path};
    std::vector<Listed> listing;
    while (!stack.empty()) {
      const std::string d = std::move(stack.back());
      stack.pop_back();
      if (!list_dir(d, listing)) continue;
      for (const Listed& c : listing) {
        const std::string cp = join(d, c.name);
        bool skip = false;
        for (const auto& ig : ignore_) skip = skip || under(cp, ig);
        if (skip) continue;
        s.extras.set(cp, make_extra(cp, c));
        if (c.dir) stack.push_back(cp);
      }
    }
  };
  auto remove = [&](const std::string& path) {
    const uint32_t c = ix.lookup(path);
    if (c != UINT32_MAX && !dead_at(s, c)) kill_subtree(s, c);
    drop_extras(s, path);
  };

  if (tree) {
    if (dir == opt_.root) return;  // the watcher asks for a full scan instead
    remove(dir);
    struct stat st {};
    if (lstat(dir.c_str(), &st) == 0 && S_ISDIR(st.st_mode)) {
      Listed self{dir.substr(dir.rfind('/') + 1), true, 0, 0,
                  uint64_t(st.st_mtimespec.tv_sec) * 1000000000 + uint64_t(st.st_mtimespec.tv_nsec), 0, false};
      add(dir, self);
    }
    return;
  }

  std::vector<Listed> listing;
  if (!list_dir(dir, listing)) {
    struct stat st {};
    if (lstat(dir.c_str(), &st) != 0) remove(dir);
    return;
  }
  if (!in_index && !in_overlay && dir != opt_.root) {
    // A folder we don't know yet: its parent's listing adds it with its
    // subtree.
    if (depth < 64 && dir.size() > opt_.root.size()) apply_dir(s, parent_of(dir), false, depth + 1);
    return;
  }

  // What we hold for this folder now: name -> index entry or overlay entry.
  struct Held {
    uint32_t e = UINT32_MAX;
    const Extra* x = nullptr;
  };
  std::unordered_map<std::string, Held> held;
  if (in_index)
    for (uint32_t c : ix.kids(e))
      if (!dead_at(s, c)) held[ix.name(c)].e = c;
  const std::string lo = dir == "/" ? "/" : dir + "/";
  for (auto it = s.extras.lower_bound(lo); it != s.extras.end() && it->first.starts_with(lo); ++it) {
    const std::string_view rest = std::string_view(it->first).substr(lo.size());
    if (rest.find('/') == std::string_view::npos) held[std::string(rest)].x = it->second.get();
  }

  for (const Listed& l : listing) {
    const std::string path = join(dir, l.name);
    bool skip = false;
    for (const auto& ig : ignore_) skip = skip || under(path, ig);
    if (skip) continue;
    const uint32_t mtime = uint32_t(std::min<uint64_t>(l.mtime / 1000000000, UINT32_MAX));
    auto it = held.find(l.name);
    if (it == held.end()) {
      add(path, l);
      continue;
    }
    const Held h = it->second;
    held.erase(it);
    if (h.e != UINT32_MAX) {
      if (ix.is_dir(h.e) != l.dir) {
        kill_subtree(s, h.e);
        add(path, l);
      } else if (!l.dir && (ix.size(h.e) != l.size || ix.mtime[h.e] != mtime || ix.flags[h.e] != l.flags)) {
        kill(s, h.e);
        s.extras.set(path, make_extra(path, l));
      }
    } else if (h.x) {
      if (h.x->dir != l.dir) {
        drop_extras(s, path);
        add(path, l);
      } else if (!l.dir && (h.x->size != l.size || h.x->mtime != mtime || h.x->flags != l.flags)) {
        s.extras.set(path, make_extra(path, l));
      }
    }
  }
  // Gone from disk.
  for (const auto& [name, h] : held) {
    if (h.e != UINT32_MAX) kill_subtree(s, h.e);
    if (h.x) drop_extras(s, join(dir, name));
  }
}

}  // namespace fplussearch
