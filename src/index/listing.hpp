#pragma once

#include <sys/attr.h>
#include <sys/stat.h>
#include <sys/vnode.h>
#include <unistd.h>

#include <cstdint>
#include <cstring>
#include <string_view>
#include <vector>

#include "index/metadata.hpp"

namespace fplussearch::index_detail {

constexpr uint32_t kUnknownEntries = UINT32_MAX;

// Calls on(name, vnode type, size, mtime in ns, BSD flags, entries) for each
// entry of the open directory `fd`; `entries` is a subdirectory's entry
// count (kUnknownEntries for files and mount points). One
// getattrlistbulk(2) call returns hundreds of entries with their
// attributes, so there is no stat per file.
template <typename On>
void read_listing(int fd, std::vector<char>& buf, On&& on) {
  attrlist al{};
  al.bitmapcount = ATTR_BIT_MAP_COUNT;
  al.commonattr =
      ATTR_CMN_RETURNED_ATTRS | ATTR_CMN_NAME | ATTR_CMN_ERROR | ATTR_CMN_OBJTYPE | ATTR_CMN_MODTIME | ATTR_CMN_FLAGS;
  al.dirattr = ATTR_DIR_ENTRYCOUNT | ATTR_DIR_MOUNTSTATUS;
  al.fileattr = ATTR_FILE_DATALENGTH;
  for (;;) {
    const int n = getattrlistbulk(fd, &al, buf.data(), buf.size(), 0);
    if (n <= 0) break;
    const char* p = buf.data();
    for (int k = 0; k < n; ++k) {
      const char* entry = p;
      uint32_t len;
      std::memcpy(&len, entry, sizeof len);
      p += len;
      // Layout per getattrlistbulk(2): length, returned attrs, error, then
      // requested attributes in bit order.
      const char* f = entry + sizeof len;
      attribute_set_t ret;
      std::memcpy(&ret, f, sizeof ret);
      f += sizeof ret;
      if (ret.commonattr & ATTR_CMN_ERROR) f += sizeof(uint32_t);
      if (!(ret.commonattr & ATTR_CMN_NAME)) continue;
      attrreference_t ref;
      std::memcpy(&ref, f, sizeof ref);
      const char* nm = f + ref.attr_dataoffset;
      const size_t nl = strnlen(nm, ref.attr_length);
      f += sizeof ref;
      uint32_t type = VNON;
      if (ret.commonattr & ATTR_CMN_OBJTYPE) {
        std::memcpy(&type, f, sizeof type);
        f += sizeof type;
      }
      uint64_t mtime = 0;
      if (ret.commonattr & ATTR_CMN_MODTIME) {
        timespec ts;
        std::memcpy(&ts, f, sizeof ts);
        f += sizeof ts;
        mtime = uint64_t(ts.tv_sec) * 1000000000ull + uint64_t(ts.tv_nsec);
      }
      uint32_t bsd_flags = 0;
      if (ret.commonattr & ATTR_CMN_FLAGS) {
        std::memcpy(&bsd_flags, f, sizeof bsd_flags);
        f += sizeof bsd_flags;
      }
      uint32_t entries = kUnknownEntries;
      if (ret.dirattr & ATTR_DIR_ENTRYCOUNT) {
        std::memcpy(&entries, f, sizeof entries);
        f += sizeof entries;
      }
      if (ret.dirattr & ATTR_DIR_MOUNTSTATUS) {
        uint32_t mount;
        std::memcpy(&mount, f, sizeof mount);
        f += sizeof mount;
        if (mount & (DIR_MNTSTATUS_MNTPOINT | DIR_MNTSTATUS_TRIGGER)) entries = kUnknownEntries;
      }
      uint64_t size = 0;
      if (ret.fileattr & ATTR_FILE_DATALENGTH) {
        off_t v;
        std::memcpy(&v, f, sizeof v);
        size = uint64_t(v);
      }
      if (nl == 0 || nl > 1023) continue;
      on(std::string_view(nm, nl), type, size, mtime, bsd_flags, entries);
    }
  }
}

inline uint8_t entry_flags(uint32_t type, uint32_t bsd_flags) {
  return uint8_t((bsd_flags & UF_HIDDEN ? kFlagHidden : 0) | (type == VLNK ? kFlagLink : 0));
}

}  // namespace fplussearch::index_detail
