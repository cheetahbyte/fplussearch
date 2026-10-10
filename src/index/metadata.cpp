#include "index/metadata.hpp"

#include <initializer_list>
#include <unordered_map>

namespace fplussearch {

namespace {

const std::unordered_map<std::string_view, uint8_t>& ext_table() {
  static const std::unordered_map<std::string_view, uint8_t> table = [] {
    std::unordered_map<std::string_view, uint8_t> m;
    auto add = [&](uint8_t c, std::initializer_list<std::string_view> xs) {
      for (auto x : xs) m.emplace(x, c);
    };
    add(CatVideo, {"mp4", "m4v", "mov", "mkv", "avi", "webm", "wmv", "flv", "mpg", "mpeg", "3gp",
                   "mts", "m2ts", "vob", "ogv", "hevc"});
    add(CatAudio, {"mp3", "wav", "flac", "aac", "m4a", "ogg", "opus", "aif", "aiff", "alac", "wma",
                   "mid", "midi", "caf"});
    add(CatImage, {"jpg", "jpeg", "png", "gif", "heic", "heif", "webp", "tif", "tiff", "bmp",
                   "svg", "ico", "icns", "raw", "cr2", "cr3", "nef", "arw", "dng", "psd", "avif"});
    add(CatDoc, {"pdf", "doc", "docx", "txt", "md", "rtf", "pages", "odt", "xls", "xlsx", "csv",
                 "ppt", "pptx", "key", "numbers", "epub", "tex"});
    add(CatArchive, {"zip", "tar", "gz", "tgz", "bz2", "xz", "7z", "rar", "dmg", "iso", "zst",
                     "pkg", "xip"});
    add(CatCode, {"c",  "cc",   "cpp",  "cxx", "h",    "hh",   "hpp",  "rs",   "go",
                  "py", "js",   "mjs",  "ts",  "tsx",  "jsx",  "java", "kt",   "swift",
                  "m",  "mm",   "rb",   "php", "sh",   "fish", "zsh",  "lua",  "cs",
                  "zig", "json", "toml", "yaml", "yml", "html", "css",  "scss", "sql"});
    add(CatFont, {"ttf", "otf", "woff", "woff2", "ttc", "dfont"});
    return m;
  }();
  return table;
}

}  // namespace

uint8_t categorize(std::string_view name) {
  size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return CatNone;
  size_t n = name.size() - dot - 1;
  if (n == 0 || n > 8) return CatNone;
  char buf[8];
  for (size_t i = 0; i < n; ++i) buf[i] = char(kFold[uint8_t(name[dot + 1 + i])]);
  auto& t = ext_table();
  auto it = t.find(std::string_view(buf, n));
  return it == t.end() ? CatNone : it->second;
}

}  // namespace fplussearch
