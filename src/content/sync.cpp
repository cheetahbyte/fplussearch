#include <sys/stat.h>

#include <algorithm>
#include <queue>

#include "content/content_index.hpp"
#include "content/document.hpp"
#include "content/eligibility.hpp"
#include "content/segment_io.hpp"

namespace fplussearch {

using namespace content_detail;

void ContentIndex::sync(const Index& ix, bool background, ScanProgress* progress) {
  std::lock_guard w(write_m_);
  // Wanted: eligible files under home, from the file index. Paths are
  // built in one reused buffer (depth-first), names read without copies.
  PathArena arena;
  std::vector<DocIn> want;
  const uint32_t home = ix.lookup(home_);
  if (home != UINT32_MAX && ix.is_dir(home)) {
    std::string path = home_;
    char buf[256 + 32];
    std::vector<std::pair<uint32_t, size_t>> stack{{home, path.size()}};  // directory, its path length
    std::vector<std::string> names{""};
    while (!stack.empty()) {
      const auto [d, len] = stack.back();
      stack.pop_back();
      path.resize(len);
      if (d != home) path += '/', path += names.back();
      names.pop_back();
      const size_t base = path.size();
      for (uint32_t c : ix.kids(d)) {
        if (ix.flags[c] & kFlagLink) continue;
        const std::string_view name = ix.name_view(c, buf);
        path.resize(base);
        path += '/';
        path += name;
        if (ix.is_dir(c)) {
          if (!skip_dir(std::string_view(path).substr(home_.size() + 1), name)) {
            stack.push_back({c, base});
            names.emplace_back(name);
          }
        } else if (file_ok(name, ix.size(c))) {
          want.push_back({arena.add(path), ix.size(c), ix.mtime[c]});
        }
      }
    }
  }
  std::sort(want.begin(), want.end(), [](const DocIn& a, const DocIn& b) { return a.path < b.path; });

  // Merge live docs in path order to diff against the sorted wanted list.
  auto cur = state();
  struct Held {
    std::string_view path;
    uint32_t seg, doc;
  };
  auto later = [](const Held& a, const Held& b) { return a.path > b.path; };
  std::priority_queue<Held, std::vector<Held>, decltype(later)> held(later);
  std::vector<uint32_t> positions(cur->segs.size(), 0);
  auto advance = [&](uint32_t si) {
    const SegView& v = cur->segs[si];
    uint32_t& k = positions[si];
    while (k < v.seg->ndocs) {
      const uint32_t d = v.seg->by_path[k++];
      if (!v.is_dead(d)) {
        held.push({v.seg->path(d), si, d});
        break;
      }
    }
  };
  for (uint32_t si = 0; si < cur->segs.size(); ++si) advance(si);
  size_t todo = 0;  // want[0, todo): the docs to (re)read, compacted in place
  std::vector<std::vector<uint64_t>> dead(cur->segs.size());
  std::vector<bool> touched(cur->segs.size(), false);
  auto kill = [&](const Held& h) {
    if (!touched[h.seg]) dead[h.seg] = *cur->segs[h.seg].dead, touched[h.seg] = true;
    dead[h.seg][h.doc >> 6] |= uint64_t(1) << (h.doc & 63);
  };
  size_t i = 0;
  while (i < want.size() || !held.empty()) {
    if (held.empty() || (i < want.size() && want[i].path < held.top().path)) {
      want[todo++] = want[i++];
      continue;
    }
    const Held h = held.top();
    held.pop();
    advance(h.seg);
    if (i >= want.size() || h.path < want[i].path) {
      kill(h);
    } else {
      const Segment& s = *cur->segs[h.seg].seg;
      // Not-text docs keep their stat from when they were read; recheck
      // them only when the file index reports a change.
      bool changed = !s.blooms || s.size[h.doc] != want[i].size || s.mtime[h.doc] != want[i].mtime;
      if (!changed && s.rank[h.doc] != kNotText) {
        struct stat current{};
        changed = stat(std::string(h.path).c_str(), &current) != 0 ||
                  time_ns(current.st_mtimespec) != (*s.blooms)[h.doc].mtime_ns ||
                  time_ns(current.st_ctimespec) != (*s.blooms)[h.doc].ctime_ns;
      }
      if (changed) {
        kill(h);
        want[todo++] = want[i];
      }
      ++i;
    }
  }
  if (std::any_of(touched.begin(), touched.end(), [](bool t) { return t; })) {
    auto next = std::make_shared<State>(*cur);
    for (size_t si = 0; si < touched.size(); ++si) {
      if (!touched[si]) continue;
      save_dead(dir_, next->segs[si].seg->id, dead[si]);
      next->segs[si].live = live_count(dead[si], next->segs[si].seg->ndocs);
      next->segs[si].dead = std::make_shared<std::vector<uint64_t>>(std::move(dead[si]));
    }
    publish(std::move(next));
  }
  cur.reset();
  if (progress) progress->source_files.store(todo, std::memory_order_relaxed);
  want.resize(todo);
  add_docs(want, background);
  std::vector<DocIn>().swap(want);
  arena.clear();
  maybe_merge();
}

}  // namespace fplussearch
