#include "index/index.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <cstdlib>

#include "index/listing.hpp"

namespace fplussearch {

using namespace index_detail;

bool has_full_disk_access() {
  // Readable only with Full Disk Access; without it, opening fails at once
  // rather than asking.
  static const bool fda = [] {
    const int fd = open("/Library/Application Support/com.apple.TCC/TCC.db", O_RDONLY | O_CLOEXEC);
    if (fd >= 0) close(fd);
    return fd >= 0;
  }();
  return fda;
}

std::vector<std::string> excluded_dirs(const std::string& root) {
  std::vector<std::string> out;
  if (root == "/") {
    // Other volumes, the Data volume mirror behind the firmlinks, and devfs.
    out = {"/System/Volumes", "/Volumes", "/dev", "/net", "/home", "/private/var/vm"};
  }
  // Without Full Disk Access, opening these asks the user and blocks until
  // they answer, so they are left out instead.
  const char* home = std::getenv("HOME");
  if (!has_full_disk_access() && home)
    for (const char* d : {"Desktop", "Documents", "Downloads", "Library/Mobile Documents", "Library/Containers",
                          "Library/Group Containers", "Library/CloudStorage", "Pictures/Photos Library.photoslibrary"})
      out.push_back(std::string(home) + "/" + d);
  return out;
}

bool list_dir(const std::string& path, std::vector<Listed>& out) {
  out.clear();
  const int fd = open(path.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  thread_local std::vector<char> buf(256 * 1024);
  read_listing(fd, buf, [&](std::string_view name, uint32_t type, uint64_t size, uint64_t mtime, uint32_t bsd_flags,
                            uint32_t) {
    const bool dir = type == VDIR;
    out.push_back({std::string(name), dir, dir ? uint8_t(0) : uint8_t(size_class(size) << 4 | categorize(name)),
                   dir ? 0 : size, mtime, entry_flags(type, bsd_flags), type == VREG});
  });
  close(fd);
  return true;
}

}  // namespace fplussearch
