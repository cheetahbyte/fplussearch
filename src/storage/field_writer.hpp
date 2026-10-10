#pragma once

#include <array>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace fplussearch {

// Writes the same container as save_fields, but field by field and in any
// order, so a big field can be streamed out instead of held in memory.
class FieldWriter {
 public:
  FieldWriter(std::string file, const char (&magic)[8], size_t count);
  ~FieldWriter();
  FieldWriter(const FieldWriter&) = delete;
  FieldWriter& operator=(const FieldWriter&) = delete;

  bool write(size_t field, const void* data, size_t size);
  // A field written in pieces: begin, append..., end.
  bool begin(size_t field);
  bool append(const void* data, size_t size);
  void end();
  // Writes the header and publishes the file; false if anything failed.
  bool finish(const std::array<uint64_t, 8>& meta);

 private:
  std::string file_, tmp_;
  char magic_[8];
  FILE* f_ = nullptr;
  std::vector<uint64_t> table_;  // (offset, size) per field
  uint64_t pos_ = 0;
  size_t open_ = SIZE_MAX;  // the field being appended to
  bool ok_ = true;
};

}  // namespace fplussearch
