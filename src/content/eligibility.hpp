#pragma once

#include <cstdint>
#include <string_view>

namespace fplussearch::content_detail {

bool skip_dir(std::string_view rel, std::string_view name);
bool file_ok(std::string_view name, uint64_t size);
int8_t doc_rank(std::string_view path);
bool path_in_scope(std::string_view path, std::string_view home);

}  // namespace fplussearch::content_detail
