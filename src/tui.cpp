#include "tui.hpp"

#include <poll.h>
#include <signal.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <deque>
#include <mutex>
#include <thread>

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

size_t display_width(std::string_view s) {
  size_t w = 0;
  for (unsigned char c : s) w += (c & 0xc0) != 0x80;
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
      : ix_(std::move(ix)), engine_(engine), opt_(opt) {
    if (const char* h = std::getenv("HOME")) home_ = h;
  }

  std::string run() {
    if (!ix_ || opt_.rescan) start_scan();
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
      if (adopt_scan()) search();
      render();

      pollfd p{STDIN_FILENO, POLLIN, 0};
      if (poll(&p, 1, scanning_ ? 100 : 1000) <= 0) continue;
      char buf[4096];
      ssize_t n = ::read(STDIN_FILENO, buf, sizeof buf);
      if (n <= 0) break;
      bool changed = false;
      for (ssize_t i = 0; i < n && !quit; ++i) {
        unsigned char c = uint8_t(buf[i]);
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
          if (ix_ && sel_ < int(res_.top.size())) chosen = ix_->path(res_.top[size_t(sel_)]);
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
      if (changed) {
        sel_ = 0;
        search(true);
      }
    }
    restore_terminal();
    if (scanner_.joinable()) scanner_.detach();
    return chosen;
  }

  bool scanning() const { return scanning_; }

 private:
  void start_scan() {
    scanning_ = true;
    const bool background = ix_ != nullptr;
    scanner_ = std::thread([this, background] {
      auto fresh = std::make_shared<Index>(
          build_index(opt_.root, opt_.scan_threads, &progress_, background, opt_.cache_file));
      std::lock_guard lk(scan_m_);
      scanned_ = std::move(fresh);
    });
  }

  bool adopt_scan() {
    std::lock_guard lk(scan_m_);
    if (!scanned_) return false;
    ix_ = std::move(scanned_);
    scanning_ = false;
    scanner_.join();
    return true;
  }

  void search(bool record = false) {
    if (!ix_) return;
    size_t limit = size_t(std::max(1, rows_ - 8));
    auto t0 = std::chrono::steady_clock::now();
    Query q = parse_query(query_);
    if (q.terms.empty() && q.exts.empty() && !q.has_size && !q.cat_mask && q.want == Want::Any) {
      res_ = {};
    } else {
      res_ = engine_.search(*ix_, q, limit);
    }
    last_ms_ = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    error_ = q.error;
    sel_ = std::min(sel_, std::max(0, int(res_.top.size()) - 1));
    if (record && !query_.empty()) {
      times_.push_back(last_ms_);
      if (times_.size() > 200) times_.pop_front();
    }
  }

  void move(int d) {
    sel_ = std::clamp(sel_ + d, 0, std::max(0, int(res_.top.size()) - 1));
  }

  std::string dir_of(uint32_t e) const {
    std::string d = ix_->path(ix_->parent(e));
    if (!home_.empty() && d.starts_with(home_) && (d.size() == home_.size() || d[home_.size()] == '/'))
      d = "~" + d.substr(home_.size());
    return d;
  }

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
    title += opt_.root == "/" ? "whole disk" : opt_.root;
    if (ix_) title += " · " + commas(ix_->count()) + " files and folders";
    if (scanning_) {
      title += ix_ ? " · refreshing " : " · indexing ";
      title += commas(progress_.entries.load(std::memory_order_relaxed));
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
    std::string q = clip(query_, inner > stats_w + 6 ? inner - stats_w - 6 : 1);
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
      line(out, row++, pad + kRed + error_);
    }
    for (size_t i = 0; ix_ && i < res_.top.size() && row <= last_row; ++i, ++row) {
      uint32_t e = res_.top[i];
      std::string name = ix_->name(e);
      if (e == 0) name = ix_->root;
      if (ix_->is_dir(e)) name += "/";
      std::string nm = clip(name, inner);
      size_t room = inner - display_width(nm);
      std::string dir = room > 4 ? clip(dir_of(e), room - 2) : "";
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

  std::thread scanner_;
  std::mutex scan_m_;
  std::shared_ptr<const Index> scanned_;
  ScanProgress progress_;
  bool scanning_ = false;
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
