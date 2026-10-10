#include "index/cache_format.hpp"

#include <algorithm>
#include <bit>
#include <utility>

namespace fplussearch::index_detail {

bool attach(Index& ix, Fields&& f) {
  Span<char> root;
  if (!f.get(kRoot, root)) return false;
  Index r;
  r.root.assign(root.p, root.n);
  r.n = uint32_t(f.meta[kMetaN]);
  r.dirs = uint32_t(f.meta[kMetaDirs]);
  r.parent_bits = uint32_t(f.meta[kMetaParentBits]);
  r.build_id = f.meta[kMetaBuildId];
  r.event_id = f.meta[kMetaEventId];
  bool ok = f.meta[kMetaN] <= UINT32_MAX && r.dirs <= r.n;
  for (size_t s = 0; s < 4; ++s) ok = ok && f.section(kSections + s * kSectionFields, r.sections[s], s % 2 == 1);
  ok = ok && f.get(kParents, r.parents) && f.get(kFileKind, r.file_kind) && f.get(kFileSize, r.file_size) &&
       f.get(kBigSizes, r.big_sizes) &&
       f.get(kCodeEntry, r.code_entry) && f.get(kCodeMtime, r.code_mtime) && r.code_entry.n == r.code_mtime.n &&
       f.get(kMtime, r.mtime) && f.get(kFlags, r.flags) && f.get(kDirPrior, r.dir_prior) &&
       f.get(kDirOrder, r.dir_order) && f.get(kDirPre, r.dir_pre) && f.get(kDirEnd, r.dir_end) &&
       r.mtime.n == r.n && r.flags.n == r.n && r.dir_prior.n == r.dirs && r.dir_order.n == r.dirs &&
       r.dir_pre.n == r.dirs && r.dir_end.n == r.dirs && f.get(kChildStart, r.child_start) &&
       f.get(kChildren, r.children) && r.child_start.n == size_t(r.dirs) + 1 && r.children.n + 1 == r.n &&
       r.file_kind.n == r.n - r.dirs && r.file_size.n == r.n - r.dirs && r.parent_bits >= 1 &&
       r.parent_bits <= 32 && r.parents.n >= (uint64_t(r.n) * r.parent_bits + 7) / 8 + 8;
  if (!ok) return false;
  r.bytes = f.bytes;
  r.backing = std::move(f.backing);
  for (auto& s : r.sections) {
    auto first = std::make_shared<std::vector<uint32_t>>(s.names());
    for (uint32_t b = 0; b < s.blocks(); ++b) {
      uint32_t e = s.block_first[b];
      const uint32_t begin = b * kBlock;
      const uint32_t end = std::min<uint32_t>(begin + kBlock, s.names());
      for (uint32_t i = begin; i < end; ++i) {
        (*first)[i] = e;
        e += s.count(i);
      }
    }
    s.name_first = std::move(first);
    auto planes = std::make_shared<std::vector<std::array<uint64_t, kBlock>>>(
        (size_t(s.names()) + kBlock - 1) / kBlock);
    for (uint32_t i = 0; i < s.names(); ++i) {
      uint64_t mask = s.mask[i];
      while (mask) {
        (*planes)[i / kBlock][std::countr_zero(mask)] |= uint64_t(1) << (i % kBlock);
        mask &= mask - 1;
      }
    }
    s.mask_planes = std::move(planes);
  }
  ix = std::move(r);
  return true;
}

}  // namespace fplussearch::index_detail

namespace fplussearch {

bool load_index(Index& ix, const std::string& file) {
  using namespace index_detail;
  Fields f;
  return map_fields(file, kMagic, kFields, f) && attach(ix, std::move(f));
}

}  // namespace fplussearch
