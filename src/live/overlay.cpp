#include "live/live.hpp"

#include <unordered_map>

namespace fplussearch {

namespace {

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
