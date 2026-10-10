#include "content/eligibility.hpp"

#include "content/content_index.hpp"
#include "content/grams.hpp"

namespace fplussearch::content_detail {

namespace {

// Folders whose contents are generated, package stores or caches: never
// the text someone searches for, and often huge. Unlike build/, vendor/,
// dist/ and nested Library/ folders, which hold real source in many trees.
constexpr std::string_view kSkipDirs[] = {
    "node_modules", ".git", "target", "DerivedData", "__pycache__", ".venv", "venv", "site-packages", "Pods", ".next",
    ".turbo", ".cache", ".build", ".rustup", ".cargo", ".npm", ".bun", ".nvm", ".pnpm-store", "coverage", ".Trash",
    ".svn", ".hg", ".gradle", ".m2", ".pyenv", ".rbenv", ".gem", ".conda", "miniconda3", "anaconda3", ".docker",
    ".orbstack", ".colima", ".lima", ".ollama", ".android", ".expo", ".terraform.d", ".wrangler", ".vscode-server",
    "cache", "Cache", "caches", "Caches"};
// Folders right under home (paths relative to it): app data and toolchains.
constexpr std::string_view kSkipUnderHome[] = {
    "Library", "go/pkg", ".cursor/extensions", ".vscode/extensions", ".local/share", ".local/state", ".config/gcloud",
    ".codex/.tmp"};
constexpr std::string_view kSkipSuffixes[] = {
    ".app", ".photoslibrary", ".library", ".lrlibrary", ".musiclibrary", ".tvlibrary", ".imovielibrary", ".xcassets",
    ".framework", ".bundle", ".xcarchive", ".xcresult", ".dSYM", ".salon", ".lrdata"};
// Extensions that are never text: not worth an open.
constexpr std::string_view kBinaryExts[] = {
    "png", "jpg", "jpeg", "gif", "heic", "heif", "webp", "tif", "tiff", "bmp", "ico", "icns", "psd", "avif", "jxl",
    "raw", "cr2", "cr3", "nef", "arw", "dng", "mp4", "mov", "m4v", "mkv", "avi", "webm", "wmv", "flv", "mpg", "mpeg",
    "mp3", "m4a", "aac", "wav", "flac", "aiff", "aif", "ogg", "opus", "caf", "mid", "midi", "zip", "tar", "gz", "tgz",
    "bz2", "xz", "7z", "rar", "dmg", "iso", "zst", "lz4", "xip", "pkg", "jar", "war", "whl", "egg", "apk", "ipa",
    "ttf", "otf", "woff", "woff2", "ttc", "eot", "o", "a", "so", "dylib", "dll", "exe", "obj", "lib", "class", "pyc",
    "pyo", "wasm", "node", "bin", "dat", "db", "sqlite", "sqlite3", "db-wal", "db-shm", "pdf", "doc", "docx", "xls",
    "xlsx", "ppt", "pptx", "pages", "numbers", "key", "odt", "ods", "epub", "pak", "car", "nib", "mlmodel",
    "onnx", "pt", "pth", "ckpt", "safetensors", "gguf", "npy", "npz", "parquet", "arrow", "feather", "h5", "hdf5",
    "tfrecord", "pb", "idx", "pack", "keystore", "p12", "der", "cer", "mobileprovision", "dex", "res", "icc",
    "ktx", "dds", "exr", "hdr", "blend", "fbx", "glb", "usdz", "unitypackage", "ai", "sketch", "fig", "xcf"};

bool eq_fold(std::string_view a, std::string_view b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i)
    if (fold(uint8_t(a[i])) != fold(uint8_t(b[i]))) return false;
  return true;
}

}  // namespace

// `rel`: the folder's path below home; `name`: its last component.
bool skip_dir(std::string_view rel, std::string_view name) {
  for (auto x : kSkipDirs)
    if (name == x) return true;
  for (auto x : kSkipUnderHome)
    if (rel == x) return true;
  for (auto x : kSkipSuffixes)
    if (name.size() > x.size() && name.ends_with(x)) return true;
  return false;
}

// The name half of eligibility: a file worth opening to see if it's text.
bool file_ok(std::string_view name, uint64_t size) {
  if (size > kMaxContentFile || size == 0) return false;
  if (name.ends_with(".min.js") || name == "package-lock.json" || name == ".DS_Store") return false;
  const size_t dot = name.rfind('.');
  if (dot == std::string_view::npos || dot == 0) return true;
  const std::string_view ext = name.substr(dot + 1);
  if (ext.size() > 16) return true;
  for (auto x : kBinaryExts)
    if (eq_fold(ext, x)) return false;
  return true;
}

// Candidate order tier: your files first, then dot-folders, logs and locks.
int8_t doc_rank(std::string_view path) {
  int8_t r = 0;
  if (path.find("/.") != std::string_view::npos) r -= 2;
  const std::string_view name = path.substr(path.rfind('/') + 1);
  for (std::string_view x : {".jsonl", ".ndjson", ".log", ".lock", ".sum"})
    if (name.ends_with(x)) return int8_t(r - 1);
  return r;
}

// Whether `path` is inside `home` and no folder on the way is skipped.
bool path_in_scope(std::string_view path, std::string_view home) {
  if (!path.starts_with(home) || (path.size() > home.size() && path[home.size()] != '/')) return false;
  if (path.size() <= home.size() + 1) return true;
  const std::string_view rel = path.substr(home.size() + 1);
  for (size_t at = 0;;) {
    const size_t slash = rel.find('/', at);
    if (slash == std::string_view::npos) return true;  // the last component is the file itself
    if (skip_dir(rel.substr(0, slash), rel.substr(at, slash - at))) return false;
    at = slash + 1;
  }
}

}  // namespace fplussearch::content_detail
