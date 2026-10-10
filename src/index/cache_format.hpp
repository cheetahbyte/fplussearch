#pragma once

#include "index/index.hpp"
#include "storage/fields.hpp"
#include "storage/section.hpp"

namespace fplussearch::index_detail {

// Field order in the cache file.
enum Field : size_t {
  kRoot = 0,
  kSections = 1,  // kSectionFields per section
  kParents = kSections + 4 * kSectionFields,
  kFileKind,
  kFileSize,
  kBigSizes,
  kCodeEntry,
  kCodeMtime,
  kMtime,
  kFlags,
  kDirPrior,
  kDirOrder,
  kDirPre,
  kDirEnd,
  kChildStart,
  kChildren,
  kFields
};
enum Meta : size_t { kMetaN, kMetaDirs, kMetaParentBits, kMetaBuildId, kMetaEventId };

constexpr char kMagic[8] = {'F', 'S', 'R', 'C', 'H', 'J', 'X', '1'};
struct Built {
  std::array<uint64_t, 8> meta{};
  std::vector<Blob> fields = std::vector<Blob>(kFields);
};

bool attach(Index& ix, Fields&& f);

}  // namespace fplussearch::index_detail
