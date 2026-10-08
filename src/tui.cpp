#include "tui.hpp"

#include "live.hpp"
#include "search.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>
#include <unordered_map>

namespace fplussearch {

namespace {

termios g_saved;
volatile sig_atomic_t g_resized = 1;

void write_all(const std::string& s) {
  size_t off = 0;
  while (off < s.size()) {
    ssize_t n = ::write(STDOUT_FILENO, s.data() + off, s.size() - off);
    if (n <= 0) return;
    off += size_t(n);
  }
}

void restore_terminal() {
  write_all("\x1b[?1049l\x1b[?25h");
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &g_saved);
}

void on_fatal_signal(int sig) {
  restore_terminal();
  signal(sig, SIG_DFL);
  raise(sig);
}

void enter_terminal() {
  tcgetattr(STDIN_FILENO, &g_saved);
  termios t = g_saved;
  t.c_iflag &= ~tcflag_t(IXON | ICRNL | BRKINT | INPCK | ISTRIP);
  t.c_lflag &= ~tcflag_t(ECHO | ICANON | ISIG | IEXTEN);
  t.c_cc[VMIN] = 1;
  t.c_cc[VTIME] = 0;
  tcsetattr(STDIN_FILENO, TCSAFLUSH, &t);
  signal(SIGWINCH, [](int) { g_resized = 1; });
  signal(SIGTERM, on_fatal_signal);
  signal(SIGHUP, on_fatal_signal);
  write_all("\x1b[?1049h");
}

std::string commas(uint64_t v) {
  std::string s = std::to_string(v);
  for (int i = int(s.size()) - 3; i > 0; i -= 3) s.insert(size_t(i), ",");
  return s;
}

std::string fmt_ms(double ms) {
  char buf[32];
  std::snprintf(buf, sizeof buf, ms < 10 ? "%.2f ms" : "%.1f ms", ms);
  return buf;
}

// Text from the filesystem made safe to print: control characters (C0, DEL,
// C1) and invalid UTF-8 become '?', so a crafted file name cannot inject
// terminal escape sequences.
std::string printable(std::string_view s) {
  std::string out;
  out.reserve(s.size());
  for (size_t i = 0; i < s.size();) {
    const unsigned char c = uint8_t(s[i]);
    if (c < 0x80) {
      out += (c < 0x20 || c == 0x7f) ? '?' : char(c);
      ++i;
      continue;
    }
    const size_t len = c >= 0xf0 ? 4 : c >= 0xe0 ? 3 : c >= 0xc2 ? 2 : 0;
    bool ok = len && i + len <= s.size();
    uint32_t cp = ok ? c & (0xff >> (len + 1)) : 0;
    for (size_t k = 1; ok && k < len; ++k) {
      const unsigned char d = uint8_t(s[i + k]);
      ok = (d & 0xc0) == 0x80;
      cp = cp << 6 | (d & 0x3f);
    }
    ok = ok && cp >= 0xa0 && cp <= 0x10ffff && !(cp >= 0xd800 && cp <= 0xdfff) &&
         !(len == 3 && cp < 0x800) && !(len == 4 && cp < 0x10000);
    if (ok) {
      out.append(s.substr(i, len));
      i += len;
    } else {
      out += '?';
      ++i;
    }
  }
  return out;
}

// Columns `s` occupies, ignoring ANSI escape sequences.
size_t display_width(std::string_view s) {
  size_t w = 0;
  for (size_t i = 0; i < s.size(); ++i) {
    if (s[i] == '\x1b') {
      while (i < s.size() && !((s[i] | 0x20) >= 'a' && (s[i] | 0x20) <= 'z' && s[i] != '\x1b' && s[i] != '[')) ++i;
      continue;
    }
    w += (uint8_t(s[i]) & 0xc0) != 0x80;
  }
  return w;
}

// Truncates to `width` columns on a UTF-8 boundary, appending an ellipsis.
std::string clip(std::string_view s, size_t width) {
  if (display_width(s) <= width) return std::string(s);
  if (width == 0) return {};
  size_t cols = 0, i = 0;
  for (; i < s.size(); ++i) {
    if ((uint8_t(s[i]) & 0xc0) != 0x80) {
      if (cols == width - 1) break;
      ++cols;
    }
  }
  return std::string(s.substr(0, i)) + "…";
}

void pop_utf8(std::string& s) {
  while (!s.empty()) {
    unsigned char c = uint8_t(s.back());
    s.pop_back();
    if ((c & 0xc0) != 0x80) break;
  }
}

double median(std::deque<double> v) {
  if (v.empty()) return 0;
  std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
  return v[v.size() / 2];
}

constexpr const char* kReset = "\x1b[0m";
constexpr const char* kBold = "\x1b[1m";
constexpr const char* kDim = "\x1b[38;5;245m";
constexpr const char* kFaint = "\x1b[38;5;240m";
constexpr const char* kGreen = "\x1b[38;5;114m";
constexpr const char* kBlue = "\x1b[38;5;111m";
constexpr const char* kRed = "\x1b[38;5;174m";
constexpr const char* kSelect = "\x1b[48;5;236m";

class App {
 public:
  App(std::shared_ptr<const Index> ix, Engine& engine, const TuiOptions& opt)
      : ix_(std::move(ix)), engine_(engine), opt_(opt), live_({.root = opt.root, .cache_file = opt.cache_file, .scan_threads = opt.scan_threads}, engine) {
    if (const char* h = std::getenv("HOME")) home_ = h;
    if (ix_) sx_ = load(*ix_);
  }

  std::string run() {
    following_ = !ix_ || opt_.rescan;
    if (following_) live_.start(ix_, [this] { changed_ = true; });
    enter_terminal();
    std::string chosen;
    bool quit = false;
    search();
    while (!quit) {
      if (g_resized) {
        g_resized = 0;
        winsize ws{};
        if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &ws) == 0 && ws.ws_row && ws.ws_col) {
          rows_ = ws.ws_row;
          cols_ = ws.ws_col;
        }
        search();
      }
      if (changed_.exchange(false)) adopt();
      render();

      pollfd p{STDIN_FILENO, POLLIN, 0};
      if (poll(&p, 1, live_.busy() ? 100 : 50) <= 0) continue;
      char buf[4096];
      ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
      if (n <= 0) break;
      bool changed = false;
      // Keys that act on results (selection, Enter) must see the results of
      // edits earlier in the same read, e.g. a pasted "query\r".
      auto settle = [&] {
        if (!changed) return;
        changed = false;
        sel_ = 0;
        search(true);
      };
      for (ssize_t i = 0; i < n && !quit; ++i) {
        unsigned char c = uint8_t(buf[i]);
        if (c == 0x1b || c == '\r' || c == '\n' || c == 0x10 || c == 0x0e) settle();
        if (c == 0x1b) {
          if (i + 2 < n && buf[i + 1] == '[') {
            if (buf[i + 2] == 'A') move(-1);
            if (buf[i + 2] == 'B') move(1);
            i += 2;
          } else if (n == 1) {
            quit = true;
          }
        } else if (c == 0x03 || c == 0x04) {
          quit = true;
        } else if (c == '\r' || c == '\n') {
          if (ix_ && sel_ < int(res_.top.size())) chosen = selected_path(size_t(sel_));
          quit = !chosen.empty();
        } else if (c == 0x10) {
          move(-1);
        } else if (c == 0x0e) {
          move(1);
        } else if (c == 0x7f || c == 0x08) {
          pop_utf8(query_), changed = true;
        } else if (c == 0x15) {
          query_.clear(), changed = true;
        } else if (c == 0x17) {
          while (!query_.empty() && query_.back() == ' ') query_.pop_back();
          while (!query_.empty() && query_.back() != ' ') query_.pop_back();
          changed = true;
        } else if (c >= 0x20) {
          query_ += char(c), changed = true;
        }
      }
      settle();
    }
    restore_terminal();
    if (following_ && !live_.busy()) live_.stop();  // a running scan is abandoned: run_tui exits at once
    return chosen;
  }

  bool scanning() const { return following_ && live_.busy(); }

 private:
  std::shared_ptr<const SymbolIndex> load(const Index& ix) const {
    auto sx = std::make_shared<SymbolIndex>();
    return load_symbols(*sx, symbol_file(opt_.cache_file), ix) ? sx : nullptr;
  }

  // Takes what the live index sees now and searches it again.
  void adopt() {
    state_ = live_.state();
    if (!state_) return;
    ix_ = state_->ix;
    sx_ = state_->sx;
    locs_.clear();
    search();
  }

  // Path of a result; for symbols with the definition's line appended.
  std::string selected_path(size_t i) {
    const uint32_t r = res_.top[i];
    if (!sym_mode_) return i < res_.extra.size() && res_.extra[i] ? res_.extra[i]->path : ix_->path(r);
    const SymbolLocation& loc = location(r);
    const std::string path = ix_->path(sx_->occ[r]);
    return loc.found ? path + ":" + std::to_string(loc.line) : path;
  }

  const SymbolLocation& location(uint32_t occurrence) {
    auto it = locs_.find(occurrence);
    if (it == locs_.end())
      it = locs_.emplace(occurrence, locate_symbol(ix_->path(sx_->occ[occurrence]), sx_->symbol(occurrence))).first;
    return it->second;
  }

  void search(bool record = false) {
    if (!ix_) return;
    size_t limit = size_t(std::max(1, rows_ - 8));
    auto t0 = std::chrono::steady_clock::now();
    Query q = parse_query(query_, home_);
    sym_mode_ = !q.syms.empty();
    if (sym_mode_) {
      res_ = sx_ ? (state_ ? live_.search(*state_, q, limit) : engine_.search_symbols(*ix_, *sx_, q, limit)) : Results{};
    } else if (!q.selects()) {
      res_ = {};
    } else {
      res_ = state_ ? live_.search(*state_, q, limit) : engine_.search(*ix_, q, limit);
    }
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    error_ = q.error;
    if (sym_mode_ && !sx_) error_ = live_.busy() ? "the symbol index is still being built" : "no symbol index yet";
    sel_ = std::min(sel_, std::max(0, int(res_.top.size()) - 1));
    if (record && !query_.empty()) {
      times_.push_back(last_ms_);
      if (times_.size() > 200) times_.pop_front();
    }
  }

  void move(int d) {
    sel_ = std::clamp(sel_ + d, 0, std::max(0, int(res_.top.size()) - 1));
  }

  std::string tilde(std::string d) const {
    if (!home_.empty() && d.starts_with(home_) && (d.size() == home_.size() || d[home_.size()] == '/'))
      d = "~" + d.substr(home_.size());
    return d;
  }

  std::string dir_of(uint32_t e) const { return tilde(ix_->path(ix_->parent(e))); }

  std::string sparkline(size_t width) const {
    static const char* bars[] = {"▁", "▂", "▃", "▄", "▅", "▆", "▇", "█"};
    size_t n = std::min(width, times_.size());
    if (n == 0) return std::string();
    double hi = *std::max_element(times_.end() - long(n), times_.end());
    std::string s;
    for (size_t i = times_.size() - n; i < times_.size(); ++i)
      s += bars[hi > 0 ? std::min(7, int(times_[i] / hi * 7.999)) : 0];
    return s;
  }

  void line(std::string& out, int row, const std::string& text) {
    out += "\x1b[" + std::to_string(row) + ";1H" + text + kReset + "\x1b[K";
  }

  void render() {
    const std::string pad = "  ";
    const size_t inner = size_t(std::max(20, cols_ - 4));
    std::string out = "\x1b[?25l";

    std::string title = std::string(kBold) + "fplussearch" + kReset + kDim + "  ";
    title += opt_.root == "/" ? "whole disk" : printable(opt_.root);
    if (ix_) title += " · " + commas(ix_->count()) + " files and folders";
    if (following_ && live_.busy()) {
      ScanProgress& progress_ = live_.progress();
      const uint64_t sources = progress_.source_files.load(std::memory_order_relaxed);
      if (sources) {
        title += " · symbols " + commas(sources) + " files";
      } else {
        title += ix_ ? " · refreshing " : " · indexing ";
        title += commas(progress_.entries.load(std::memory_order_relaxed));
      }
    }
    line(out, 2, pad + title);

    std::string stats;
    size_t stats_w = 0;
    if (ix_ && !query_.empty()) {
      std::string count = commas(res_.total) + " matches  ";
      std::string ms = fmt_ms(last_ms_);
      stats = std::string(kDim) + count + kGreen + ms;
      stats_w = display_width(count) + display_width(ms);
    }
    std::string q = clip(printable(query_), inner > stats_w + 6 ? inner - stats_w - 6 : 1);
    size_t qw = display_width(q);
    std::string prompt = pad + kBlue + "❯ " + kReset + kBold + q + kReset;
    size_t gap = inner > qw + 2 + stats_w ? inner - qw - 2 - stats_w : 1;
    line(out, 4, prompt + std::string(gap, ' ') + stats);

    std::string rule;
    for (size_t i = 0; i < inner; ++i) rule += "─";
    line(out, 5, pad + kFaint + rule);

    int row = 6;
    const int last_row = rows_ - 3;
    if (!ix_) {
      line(out, row++, pad + kDim + "building index…");
    } else if (!error_.empty()) {
      line(out, row++, pad + kRed + printable(error_));
    }
    for (size_t i = 0; ix_ && i < res_.top.size() && row <= last_row; ++i, ++row) {
      uint32_t e = res_.top[i];
      std::string name, dir;
      if (sym_mode_) {
        const SymbolLocation& loc = location(e);
        name = printable(sx_->symbol(e)) + "  " + kDim + (loc.found ? kind_name(loc.kind) : "?") + kReset;
        e = sx_->occ[e];
        dir = printable(dir_of(e) + "/" + ix_->name(e)) + (loc.found ? ":" + std::to_string(loc.line) : "");
      } else if (const Extra* x = i < res_.extra.size() ? res_.extra[i] : nullptr) {
        const size_t slash = x->path.rfind('/');
        name = printable(x->path.substr(slash + 1));
        if (x->dir) name += "/";
        dir = printable(tilde(slash == 0 ? "/" : x->path.substr(0, slash)));
      } else {
        name = printable(e == 0 ? ix_->root : ix_->name(e));
        if (ix_->is_dir(e)) name += "/";
        dir = printable(dir_of(e));
      }
      std::string nm = sym_mode_ ? name : clip(name, inner);
      size_t room = inner > display_width(nm) ? inner - display_width(nm) : 0;
      dir = room > 4 ? clip(dir, room - 2) : "";
      bool selected = int(i) == sel_;
      std::string bg = selected ? kSelect : "";
      std::string text = bg + pad + kReset + bg + nm + (dir.empty() ? "" : "  ") + kDim + dir;
      if (selected) {
        size_t used = display_width(nm) + (dir.empty() ? 0 : 2 + display_width(dir));
        text += std::string(inner > used ? inner - used : 0, ' ');
      }
      line(out, row, text);
    }
    for (; row <= last_row; ++row) line(out, row, "");

    std::string label = "every keystroke is a whole-disk search";
    std::string med = fmt_ms(median(times_));
    size_t spark_w = inner > label.size() + 24 ? std::min<size_t>(40, inner - label.size() - 24) : 0;
    std::string spark = sparkline(spark_w);
    std::string foot = pad + kDim + label + "   " + kGreen + spark;
    if (!times_.empty()) foot += "  " + std::string(kDim) + "median " + kGreen + med;
    line(out, rows_ - 1, foot);
    line(out, rows_, "");

    out += "\x1b[4;" + std::to_string(3 + 2 + qw + 1) + "H\x1b[?25h";
    write_all(out);
  }

  std::shared_ptr<const Index> ix_;
  Engine& engine_;
  const TuiOptions& opt_;
  std::string home_;
  std::string query_;
  std::string error_;
  Results res_;
  double last_ms_ = 0;
  std::deque<double> times_;
  int sel_ = 0;
  int rows_ = 24, cols_ = 80;

  std::shared_ptr<const SymbolIndex> sx_;
  bool sym_mode_ = false;
  std::unordered_map<uint32_t, SymbolLocation> locs_;  // per occurrence, for visible rows

  Live live_;
  bool following_ = false;
  std::shared_ptr<const Live::State> state_;  // what results refer to
  std::atomic<bool> changed_{false};
};

}  // namespace

std::string run_tui(std::shared_ptr<const Index> ix, Engine& engine, const TuiOptions& opt) {
  App app(std::move(ix), engine, opt);
  std::string chosen = app.run();
  if (app.scanning()) {
    // The background scan cannot be interrupted; skip destructors and exit.
    if (!chosen.empty()) std::printf("%s\n", chosen.c_str());
    std::fflush(stdout);
    std::_Exit(chosen.empty() ? 1 : 0);
  }
  return chosen;
}

}  // namespace fplussearch
