#include <dirent.h>
#include <sys/stat.h>

#include <algorithm>

#include "content/content_index.hpp"
#include "content/document.hpp"
#include "content/eligibility.hpp"
#include "content/segment_io.hpp"

namespace fplussearch {

using namespace content_detail;

void ContentIndex::update_paths(const std::vector<std::string>& dirs, const std::vector<std::string>& trees) {
  std::lock_guard w(write_m_);
  PathArena arena;
  std::vector<DocIn> want;
  std::vector<std::pair<std::string, bool>> areas;  // (folder, whole subtree)
  for (const auto& d : dirs) areas.push_back({d, false});
  for (const auto& d : trees) areas.push_back({d, true});
  // What's on disk now.
  for (auto& [dir, recursive] : areas) {
    while (dir.size() > 1 && dir.back() == '/') dir.pop_back();
    if (!path_in_scope(dir + "/x", home_)) continue;
    std::vector<std::string> stack{dir};
    while (!stack.empty()) {
      const std::string d = std::move(stack.back());
      stack.pop_back();
      DIR* h = opendir(d.c_str());
      if (!h) continue;
      while (dirent* e = readdir(h)) {
        const std::string_view name = e->d_name;
        if (name == "." || name == "..") continue;
        const std::string p = d + "/" + std::string(name);
        struct stat st{};
        if (lstat(p.c_str(), &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
          if (recursive && !skip_dir(std::string_view(p).substr(home_.size() + 1), name)) stack.push_back(p);
        } else if (S_ISREG(st.st_mode) && file_ok(name, uint64_t(st.st_size))) {
          want.push_back({arena.add(p), uint64_t(st.st_size), uint32_t(st.st_mtimespec.tv_sec),
                          time_ns(st.st_mtimespec), time_ns(st.st_ctimespec)});
        }
      }
      closedir(h);
    }
  }
  std::sort(want.begin(), want.end(), [](const DocIn& a, const DocIn& b) { return a.path < b.path; });
  want.erase(std::unique(want.begin(), want.end(), [](const DocIn& a, const DocIn& b) { return a.path == b.path; }),
             want.end());
  // What we hold there: direct children (or the whole subtree).
  auto cur = state();
  std::vector<std::vector<uint64_t>> dead(cur->segs.size());
  std::vector<bool> touched(cur->segs.size(), false);
  std::vector<bool> held(want.size(), false);
  for (uint32_t si = 0; si < cur->segs.size(); ++si) {
    const SegView& v = cur->segs[si];
    const Segment& s = *v.seg;
    for (const auto& [dir, recursive] : areas) {
      const std::string lo = dir + "/";
      const uint32_t* bp = s.by_path.p;
      const uint32_t* it = std::lower_bound(bp, bp + s.ndocs, lo, [&](uint32_t d, const std::string& x) { return s.path(d) < x; });
      for (; it != bp + s.ndocs && s.path(*it).starts_with(lo); ++it) {
        const uint32_t d = *it;
        const std::string_view p = s.path(d);
        if (!recursive && p.find('/', lo.size()) != std::string_view::npos) continue;
        if (v.is_dead(d)) continue;
        const auto w = std::lower_bound(want.begin(), want.end(), p, [](const DocIn& a, std::string_view x) { return a.path < x; });
        if (s.blooms && w != want.end() && w->path == p && w->size == s.size[d] &&
            w->mtime_ns == (*s.blooms)[d].mtime_ns && w->ctime_ns == (*s.blooms)[d].ctime_ns) {
          held[size_t(w - want.begin())] = true;
          continue;
        }
        if (!touched[si]) dead[si] = *v.dead, touched[si] = true;
        dead[si][d >> 6] |= uint64_t(1) << (d & 63);
      }
    }
  }
  std::vector<DocIn> todo;
  for (size_t k = 0; k < want.size(); ++k)
    if (!held[k]) todo.push_back(std::move(want[k]));
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
  add_docs(todo, false);
  maybe_merge();
}

}  // namespace fplussearch
