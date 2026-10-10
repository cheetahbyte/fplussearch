#include <unistd.h>

#include <cmath>

#include "content/content_index.hpp"
#include "content/segment_io.hpp"

namespace fplussearch {

using namespace content_detail;

namespace {

constexpr size_t kMergeTier = 8;  // segments of a size tier merged into one
constexpr uint64_t kMergeCap = uint64_t(256) << 20;  // largest merge, in posting bytes
constexpr size_t kMaxSegments = 6;

}  // namespace

// Merges segments so a query visits few of them: kMergeTier segments of
// one size tier become one (updates never pile up small segments), and
// then the smallest are merged while there are more than kMaxSegments.
void ContentIndex::maybe_merge() {
  for (;;) {
    auto cur = state();
    std::vector<std::vector<size_t>> tiers(64);
    for (size_t i = 0; i < cur->segs.size(); ++i) {
      const size_t plen = std::max<size_t>(1, cur->segs[i].seg->post.n);
      tiers[size_t(std::log(double(plen)) / std::log(4.0))].push_back(i);
    }
    std::vector<size_t> group;
    for (auto& t : tiers) {
      if (t.size() < kMergeTier) continue;
      uint64_t bytes = 0;
      for (size_t j = 0; j < kMergeTier; ++j) bytes += cur->segs[t[j]].seg->post.n;
      if (bytes <= kMergeCap) {
        group.assign(t.begin(), t.begin() + kMergeTier);
        break;
      }
    }
    if (group.empty() && cur->segs.size() > kMaxSegments) {
      std::vector<size_t> by_size(cur->segs.size());
      for (size_t i = 0; i < by_size.size(); ++i) by_size[i] = i;
      std::sort(by_size.begin(), by_size.end(),
                [&](size_t a, size_t b) { return cur->segs[a].seg->post.n < cur->segs[b].seg->post.n; });
      uint64_t bytes = 0;
      for (size_t i : by_size) {
        if (group.size() >= 2 && bytes + cur->segs[i].seg->post.n > kMergeCap) break;
        bytes += cur->segs[i].seg->post.n;
        group.push_back(i);
      }
      if (group.size() < 2) group.clear();
    }
    if (group.empty()) return;
    std::vector<DocMeta> meta;
    std::vector<BloomView> blooms;
    bool all_blooms = true;
    std::vector<std::vector<uint32_t>> remap;
    for (size_t gi : group) {
      const SegView& v = cur->segs[gi];
      std::vector<uint32_t> r(v.seg->ndocs, UINT32_MAX);
      for (uint32_t d = 0; d < v.seg->ndocs; ++d) {
        if (v.is_dead(d)) continue;
        r[d] = uint32_t(meta.size());
        meta.push_back({v.seg->path(d), v.seg->size[d], v.seg->mtime[d], v.seg->rank[d]});
        blooms.push_back(v.seg->blooms ? (*v.seg->blooms)[d] : BloomView{});
        all_blooms = all_blooms && bool(v.seg->blooms);
      }
      remap.push_back(std::move(r));
    }
    std::vector<size_t> pos(group.size(), 0);
    std::vector<uint32_t> part;
    std::vector<uint8_t> part_masks;
    const uint64_t id = next_id();
    std::vector<uint32_t> list;
    std::vector<uint8_t> masks;
    const bool ok = !meta.empty() && write_segment(dir_, id, meta, [&](std::span<const uint32_t>& view, std::span<const uint8_t>& mask_view, uint32_t& t) {
      list.clear();
      masks.clear();
      for (;;) {
        bool any = false;
        t = UINT32_MAX;
        for (size_t g = 0; g < group.size(); ++g) {
          const Segment& s = *cur->segs[group[g]].seg;
          if (pos[g] < s.key.n) any = true, t = std::min(t, s.key[pos[g]]);
        }
        if (!any) return false;
        for (size_t g = 0; g < group.size(); ++g) {
          const Segment& s = *cur->segs[group[g]].seg;
          if (pos[g] >= s.key.n || s.key[pos[g]] != t) continue;
          part.clear();
          part_masks.clear();
          s.list(pos[g]++, part, 0, &part_masks);
          for (size_t i = 0; i < part.size(); ++i)
            if (remap[g][part[i]] != UINT32_MAX) list.push_back(remap[g][part[i]]), masks.push_back(part_masks[i]);
        }
        if (!list.empty()) {
          view = list;
          mask_view = masks;
          return true;
        }
      }
    }, all_blooms ? std::span<const BloomView>(blooms) : std::span<const BloomView>());
    auto seg = ok ? map_segment(dir_, id) : nullptr;
    // Writers are serialized, so nothing was killed in the group meanwhile.
    auto next = std::make_shared<State>();
    std::vector<uint64_t> gone;
    for (size_t i = 0; i < cur->segs.size(); ++i) {
      if (std::find(group.begin(), group.end(), i) == group.end()) next->segs.push_back(cur->segs[i]);
      else gone.push_back(cur->segs[i].seg->id);
    }
    if (seg) {
      auto dead = std::make_shared<std::vector<uint64_t>>((seg->ndocs + 63) / 64, 0);
      const size_t live = seg->ndocs;
      next->segs.push_back({std::move(seg), std::move(dead), live});
    } else if (!meta.empty()) {
      return;  // could not write: keep what we have
    }
    save_manifest(*next);
    publish(std::move(next));
    for (uint64_t g : gone)
      unlink(seg_file(dir_, g, "fpc").c_str()), unlink(seg_file(dir_, g, "dead").c_str()), unlink(seg_file(dir_, g, "blm").c_str());
  }
}

}  // namespace fplussearch
