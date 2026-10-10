#include "storage/field_writer.hpp"
#include "storage/format.hpp"

#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <cstring>
#include <utility>

namespace fplussearch {

FieldWriter::FieldWriter(std::string file, const char (&magic)[8], size_t count)
    : file_(std::move(file)), table_(2 * count, 0) {
  std::memcpy(magic_, magic, sizeof magic_);
  tmp_ = file_ + ".XXXXXX";
  const int fd = mkstemp(tmp_.data());
  if (fd < 0) {
    ok_ = false;
    return;
  }
  fchmod(fd, 0644);
  f_ = fdopen(fd, "wb");
  if (!f_) {
    close(fd);
    ok_ = false;
    return;
  }
  static const char zeros[64] = {};
  pos_ = align64(sizeof(Header) + table_.size() * sizeof(uint64_t));
  for (uint64_t left = pos_; left > 0;) {  // header and table, filled in by finish()
    const size_t n = std::min<uint64_t>(left, sizeof zeros);
    ok_ = ok_ && std::fwrite(zeros, 1, n, f_) == n;
    left -= n;
  }
}

FieldWriter::~FieldWriter() {
  if (f_) {
    std::fclose(f_);
    std::remove(tmp_.c_str());
  }
}

bool FieldWriter::begin(size_t field) {
  static const char zeros[64] = {};
  const uint64_t at = align64(pos_);
  ok_ = ok_ && f_ && field < table_.size() / 2 && std::fwrite(zeros, 1, at - pos_, f_) == at - pos_;
  pos_ = at;
  open_ = field;
  table_[2 * field] = pos_;
  table_[2 * field + 1] = 0;
  return ok_;
}

bool FieldWriter::append(const void* data, size_t size) {
  ok_ = ok_ && open_ != SIZE_MAX && std::fwrite(data, 1, size, f_) == size;
  pos_ += size;
  if (open_ != SIZE_MAX) table_[2 * open_ + 1] += size;
  return ok_;
}

void FieldWriter::end() { open_ = SIZE_MAX; }

bool FieldWriter::write(size_t field, const void* data, size_t size) {
  begin(field);
  append(data, size);
  end();
  return ok_;
}

bool FieldWriter::finish(const std::array<uint64_t, 8>& meta) {
  if (!f_) return false;
  Header h{};
  std::memcpy(h.magic, magic_, sizeof h.magic);
  h.count = table_.size() / 2;
  std::memcpy(h.meta, meta.data(), sizeof h.meta);
  ok_ = ok_ && std::fseek(f_, 0, SEEK_SET) == 0 && std::fwrite(&h, sizeof h, 1, f_) == 1 &&
        std::fwrite(table_.data(), sizeof(uint64_t), table_.size(), f_) == table_.size();
  ok_ = std::fclose(f_) == 0 && ok_;
  f_ = nullptr;
  if (!ok_ || std::rename(tmp_.c_str(), file_.c_str()) != 0) {
    std::remove(tmp_.c_str());
    return false;
  }
  return true;
}

}  // namespace fplussearch
