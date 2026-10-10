#pragma once

#include <sys/stat.h>

#include "storage/mapped_array.hpp"

namespace fplussearch::content_detail {

bool read_file(const char* path, BigVec<char>& buf, size_t cap, struct stat* out = nullptr, bool check_binary = false);
bool looks_binary(const BigVec<char>& b);

}  // namespace fplussearch::content_detail
