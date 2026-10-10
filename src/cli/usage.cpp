#include "cli/usage.hpp"

#include <cstdio>

namespace fplussearch::cli {

void usage() {
  std::fprintf(stderr,
               "usage: fplussearch [options] [query]\n"
               "       fplussearch serve | stdio | status | install [--login] | uninstall\n"
               "\n"
               "  no query       show this help and exit\n"
               "  query          print matching paths and exit (asks the daemon when it runs,\n"
               "                 otherwise answers from the cache and starts the daemon)\n"
               "  serve          run the daemon in the foreground: keeps the index current and\n"
               "                 answers JSON lines on a socket\n"
               "  stdio          JSON lines on stdin/stdout, through the daemon\n"
               "  status         the daemon's status\n"
               "  install        copy to ~/.local/bin; --login also starts the daemon at login\n"
               "  uninstall      remove the login item (keeps the index)\n"
               "\n"
               "  --root PATH    directory to index (default /)\n"
               "  --reindex      ignore the cached index and rescan first\n"
               "  --no-rescan    use the cached index without following changes\n"
               "  --no-daemon    don't use or start the daemon\n"
               "  --threads N    search threads (default: performance cores)\n"
               "  --scan-threads N  indexing threads (default: 8)\n"
               "  -n N           max results to print (default 50, 0 = all)\n"
               "  --bench        time each query (or a default set) over many runs\n"
               "\n"
               "query syntax: words match names fuzzily (5+ letters forgive one typo), in any\n"
               "  folder of the path; 'exact ^prefix suffix$ !exclude \"with space\"\n"
               "  ext:mp4,mov type:video|audio|image|doc|archive|code|font|app|dir|file\n"
               "  kind:file|dir|link in:~/path size:>1gb mtime:<7d re:regex path:regex limit:N\n"
               "  sym:name (definitions in source files) grep:text regex:pattern (file contents)\n");
}

}  // namespace fplussearch::cli
