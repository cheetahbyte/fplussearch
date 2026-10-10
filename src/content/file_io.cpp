#include "content/file_io.hpp"

#include <fcntl.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>

namespace fplussearch::content_detail {

// Opens a regular file without blocking (O_NONBLOCK: a FIFO never hangs
// open) and reads up to `cap` bytes into `buf`.
bool read_file(const char* path, BigVec<char>& buf, size_t cap, struct stat* out, bool check_binary) {
  const int fd = open(path, O_RDONLY | O_NONBLOCK | O_NOFOLLOW | O_CLOEXEC);
  if (fd < 0) return false;
  constexpr size_t kInitChunk = 64 * 1024;
  const size_t initial_want = std::min(cap, kInitChunk);
  buf.resize(initial_want);
  ssize_t n = read(fd, buf.data(), initial_want);
  if (n <= 0) {
    // Check if it's not a regular file or empty
    struct stat st{};
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
      close(fd);
      return false;
    }
    if (out) *out = st;
    close(fd);
    buf.clear();
    return true;
  }
  size_t got = size_t(n);
  if (check_binary && std::memchr(buf.data(), 0, std::min<size_t>(got, 8192)) != nullptr) {
    close(fd);
    return false;
  }
  if (got < initial_want) {
    // Entire file fit in initial chunk without needing fstat
    if (out) {
      if (fstat(fd, out) != 0 || !S_ISREG(out->st_mode)) {
        close(fd);
        return false;
      }
    }
    close(fd);
    buf.resize(got);
    return true;
  }
  // File is larger than initial chunk, query exact size with fstat
  struct stat st{};
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
    close(fd);
    return false;
  }
  if (out) *out = st;
  const size_t want = std::min<uint64_t>(uint64_t(st.st_size), cap);
  buf.resize(want);
  while (got < want) {
    const ssize_t nr = read(fd, buf.data() + got, want - got);
    if (nr <= 0) break;
    got += size_t(nr);
  }
  close(fd);
  buf.resize(got);
  return true;
}

bool looks_binary(const BigVec<char>& b) {
  return std::memchr(b.data(), 0, std::min<size_t>(b.size(), 8192)) != nullptr;
}

}  // namespace fplussearch::content_detail
