#include "cli/installation.hpp"
#include "cli/runtime.hpp"

#include <spawn.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <vector>

extern char** environ;

namespace fplussearch::cli {

namespace {

constexpr const char* kAgent = "com.fplussearch.daemon";

int run_launchctl(const std::vector<std::string>& args) {
  std::vector<char*> argv;
  argv.push_back(const_cast<char*>("/bin/launchctl"));
  for (const auto& a : args) argv.push_back(const_cast<char*>(a.c_str()));
  argv.push_back(nullptr);
  pid_t pid;
  if (posix_spawn(&pid, "/bin/launchctl", nullptr, nullptr, argv.data(), environ) != 0) return -1;
  int st = 0;
  waitpid(pid, &st, 0);
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

}  // namespace

int install(bool login) {
  const std::string bin_dir = home() + "/.local/bin", bin = bin_dir + "/fplussearch";
  mkdir((home() + "/.local").c_str(), 0755);
  mkdir(bin_dir.c_str(), 0755);
  std::error_code ec;
  std::filesystem::copy_file(exe_path(), bin, std::filesystem::copy_options::overwrite_existing, ec);
  if (ec) {
    std::fprintf(stderr, "copy to %s: %s\n", bin.c_str(), ec.message().c_str());
    return 1;
  }
  std::printf("installed %s\n", bin.c_str());
  if (!login) return 0;
  const std::string agents = home() + "/Library/LaunchAgents", plist = agents + "/" + kAgent + ".plist";
  mkdir(agents.c_str(), 0755);
  const std::string log = log_path(cache_path("/"));
  FILE* f = std::fopen(plist.c_str(), "w");
  if (!f) {
    std::fprintf(stderr, "cannot write %s\n", plist.c_str());
    return 1;
  }
  std::fprintf(f,
               "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n"
               "<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" "
               "\"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n"
               "<plist version=\"1.0\"><dict>\n"
               "  <key>Label</key><string>%s</string>\n"
               "  <key>ProgramArguments</key><array><string>%s</string><string>serve</string></array>\n"
               "  <key>RunAtLoad</key><true/>\n"
               "  <key>KeepAlive</key><true/>\n"
               "  <key>ProcessType</key><string>Background</string>\n"
               "  <key>StandardOutPath</key><string>%s</string>\n"
               "  <key>StandardErrorPath</key><string>%s</string>\n"
               "</dict></plist>\n",
               kAgent, bin.c_str(), log.c_str(), log.c_str());
  std::fclose(f);
  const std::string domain = "gui/" + std::to_string(getuid());
  run_launchctl({"bootout", domain + "/" + kAgent});
  if (run_launchctl({"bootstrap", domain, plist}) != 0) {
    std::fprintf(stderr, "launchctl bootstrap failed; the agent starts at next login\n");
    return 1;
  }
  std::printf("the daemon now starts at login (%s)\n", plist.c_str());
  std::printf("give %s Full Disk Access in System Settings > Privacy & Security to index protected folders\n",
              bin.c_str());
  return 0;
}

int uninstall() {
  const std::string plist = home() + "/Library/LaunchAgents/" + kAgent + ".plist";
  run_launchctl({"bootout", "gui/" + std::to_string(getuid()) + "/" + kAgent});
  std::remove(plist.c_str());
  std::printf("removed the login item; the index stays in %s\n",
              cache_path("/").substr(0, cache_path("/").rfind('/')).c_str());
  return 0;
}

}  // namespace fplussearch::cli
