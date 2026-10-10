#include "content/content_index.hpp"

#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstdio>
#include <unordered_set>

#include "content/eligibility.hpp"
#include "content/segment_io.hpp"
#include "support/pool.hpp"

namespace fplussearch {

using namespace content_detail;

ContentIndex::ContentIndex(std::string dir, std::string home, unsigned readers)
    : dir_(std::move(dir)), home_(std::move(home)), state_(std::make_shared<State>()),
      readers_(std::make_unique<Pool>(std::max(1u, readers))) {
  while (home_.size() > 1 && home_.back() == '/') home_.pop_back();
}

ContentIndex::~ContentIndex() = default;

std::shared_ptr<const ContentIndex::State> ContentIndex::state() const {
  std::lock_guard lk(m_);
  return state_;
}

void ContentIndex::publish(std::shared_ptr<const State> s) {
  std::lock_guard lk(m_);
  state_ = std::move(s);
}

uint64_t ContentIndex::next_id() { return next_id_.fetch_add(1); }

void ContentIndex::save_manifest(const State& s) const {
  std::string text;
  for (const SegView& v : s.segs) text += std::to_string(v.seg->id) + "\n";
  const std::string file = dir_ + "/manifest", tmp = file + ".tmp";
  FILE* f = std::fopen(tmp.c_str(), "w");
  if (!f) return;
  const bool ok = std::fwrite(text.data(), 1, text.size(), f) == text.size();
  if (std::fclose(f) == 0 && ok) rename(tmp.c_str(), file.c_str());
  else unlink(tmp.c_str());
}

bool ContentIndex::open() {
  std::lock_guard w(write_m_);
  mkdir(dir_.c_str(), 0755);
  auto s = std::make_shared<State>();
  std::unordered_set<std::string> keep;
  if (FILE* f = std::fopen((dir_ + "/manifest").c_str(), "r")) {
    unsigned long long id;
    while (std::fscanf(f, "%llu", &id) == 1) {
      auto seg = map_segment(dir_, id);
      if (!seg) continue;
      auto dead = std::make_shared<std::vector<uint64_t>>(load_dead(dir_, id, seg->ndocs));
      const size_t live = live_count(*dead, seg->ndocs);
      s->segs.push_back({std::move(seg), std::move(dead), live});
      keep.insert(seg_file("", id, "fpc").substr(1));
      keep.insert(seg_file("", id, "blm").substr(1));
      keep.insert(seg_file("", id, "dead").substr(1));
      next_id_ = std::max<uint64_t>(next_id_, id + 1);
    }
    std::fclose(f);
  }
  // Segments a crashed build left behind are garbage.
  if (DIR* d = opendir(dir_.c_str())) {
    while (dirent* e = readdir(d)) {
      const std::string name = e->d_name;
      if (name.starts_with("seg-") && !keep.contains(name)) unlink((dir_ + "/" + name).c_str());
    }
    closedir(d);
  }
  const bool any = !s->segs.empty();
  publish(std::move(s));
  return any;
}

bool ContentIndex::covers(std::string_view path) const { return path_in_scope(std::string(path) + "/x", home_); }

size_t ContentIndex::docs() const {
  size_t n = 0;
  for (const SegView& v : state()->segs) n += v.live;
  return n;
}

size_t ContentIndex::bytes() const {
  size_t n = 0;
  for (const SegView& v : state()->segs) n += v.seg->bytes;
  return n;
}

bool ContentIndex::needs_bloom_refresh() const {
  const auto current = state();
  const std::string prefix = home_ == "/" ? home_ : home_ + "/";
  for (const SegView& v : current->segs) {
    if (v.seg->blooms || v.live == 0) continue;
    const Segment& s = *v.seg;
    const auto begin = s.by_path.p, end = begin + s.ndocs;
    auto it = std::lower_bound(begin, end, prefix, [&](uint32_t d, const std::string& p) { return s.path(d) < p; });
    for (; it != end && s.path(*it).starts_with(prefix); ++it)
      if (!v.is_dead(*it) && s.rank[*it] != kNotText) return true;
  }
  return false;
}

}  // namespace fplussearch
