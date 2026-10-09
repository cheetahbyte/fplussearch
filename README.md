# fplussearch

Whole-disk file name and code symbol search for macOS in C++. Every keystroke
searches every file and folder on the disk, or every function and type
defined in its source files.

Inspired by [Noah's Rust file search engine](https://x.com/itsnoahd/status/2107993727809855570). Repository: https://github.com/noahdunnagan/fsearch

The code in this repository is entirely AI-generated.

## Install with Homebrew

This repository also serves as a custom Homebrew tap through `Formula/`.
The engine requires macOS on Apple Silicon.

```sh
brew tap cheetahbyte/fplussearch https://github.com/cheetahbyte/fplussearch.git
brew install --HEAD cheetahbyte/fplussearch/fplussearch
```

The formula builds from `main`; no tagged release is available yet.
These commands become available after the formula is published to the repository.

Swift clients start the shared daemon automatically. To start it at login:

```sh
brew services start cheetahbyte/fplussearch/fplussearch
```

Run services without `sudo`. Don't also run `fplussearch install --login`,
which installs a separate binary and login item.
If a daemon is already running, stop it before enabling the Homebrew service.
To stop the Homebrew service:

```sh
brew services stop cheetahbyte/fplussearch/fplussearch
```

For protected folders, grant the installed executable Full Disk Access in
System Settings > Privacy & Security. Installation doesn't grant access.

## Build

Use macOS on Apple Silicon, a C++20 compiler, PCRE2, and `pkg-config`:

```sh
brew install pcre2 pkgconf
make            # produces build/fplussearch
make lib        # produces build/libfplussearch.a for C++ embedding
make test-search # runs name-matching regression checks
```

The library's entry point is `src/live.hpp`. Link with CoreServices and
PCRE2; the Swift package doesn't require linking the C++ library.

## Use from Swift

Add this repository as a Swift package dependency and select the `FPlusSearch`
library product. The package talks to the shared daemon through
`fplussearch stdio`; install the executable separately with Homebrew or `make`.

See [Swift integration](docs/swift.md) for configuration, asynchronous search,
index readiness, lifecycle, and sandbox limitations.

## Daemon protocol

`fplussearch stdio --root PATH` starts or connects to the daemon for `PATH`.
The default root is `/`. Send one JSON object per line; each response is one
JSON object per line. String request IDs are echoed in responses.

```json
{"id":"1","op":"status"}
{"id":"2","q":"readme ext:md","limit":20}
{"id":"3","q":"sym:parse","limit":20}
{"id":"4","op":"grep","pattern":"TODO","mode":"literal","limit":20}
```

Responses include `ok`; failures include `error`. Status includes `ready`,
`busy`, and `content_pending`. Searches can fail during the first index build.
Grep responses include `complete`; budget-limited results might be partial.
Disconnecting a client doesn't stop the daemon.

The protocol is currently unversioned. Use matching client and executable
revisions until releases define compatibility guarantees.

## Usage

```sh
build/fplussearch                          # interactive; Enter prints the selected path
build/fplussearch 'size:>1gb type:video'   # print matches and exit
build/fplussearch --bench                  # timing table
```

| Option | Meaning |
| --- | --- |
| `--root PATH` | Directory to index (default `/`) |
| `--reindex` | Ignore the cached index and rescan first |
| `--no-rescan` | Use the cached index without refreshing it |
| `--threads N` | Search threads (default: all cores) |
| `--scan-threads N` | Indexing threads (default: 8) |
| `-n N` | Results to print in query mode (default 50, `0` = all) |

Interactive keys: arrows or Ctrl-P/Ctrl-N to select, Enter to print the path
and exit, Ctrl-U to clear, Ctrl-W to delete a word, Esc or Ctrl-C to quit.

The first run indexes the disk and caches the index in
`~/Library/Caches/fplussearch/`: about 25 s for 6 million entries, plus about
45 s to read 2 million source files for the symbol index. Later runs map the
cache in a few milliseconds. The interactive mode then asks FSEvents which
folders changed since the cache was written and refreshes the index in the
background, re-reading only those folders and the source files in them whose
size or modification time changed: about 2 s for the whole disk instead of a
full rescan. While it stays open, it applies new changes at most every 30 s.
A full rescan happens only when FSEvents has lost track of changes.

Give your terminal Full Disk Access to index protected folders such as Mail
and Messages. Other volumes (`/Volumes`) are not indexed.

## Query syntax

All parts must match. Name matching uses case-insensitive substrings, not scattered letters.
Words of at least five bytes also allow one wrong, extra, missing, or swapped letter.
Typo matching preserves digits. At word starts, it also preserves the first letter;
whole-name typo matches can edit the first letter.
Whole-name, stem, prefix, and word-boundary matches receive ranking bonuses; typos receive a penalty.
Prefix a word with `'` to disable typo matching, or `!` to exclude a literal substring.

| Query | Matches |
| --- | --- |
| `report 2024` | Names containing both `report` and `2024` |
| `"my file"` | Names containing `my file` |
| `size:>1gb`, `size:<=10mb`, `size:500k` | File size (`>`, `>=`, `<`, `<=`, `=`; bare means `>=`; units b, k, m, g, t, base 1024) |
| `type:video` | `video`, `audio`, `image`, `doc`, `archive`, `code` by extension, plus `dir` and `file`; comma-separate for any of several (`type:dir,video`) |
| `ext:mp4,mov` | Names ending in any of the extensions |
| `sym:parse` | Functions, types, macros and modules whose name contains `parse`, defined in source files; other parts of the query filter the files (`sym:render ext:tsx`) |

In symbol results, Enter prints `path:line`.

## Performance

Median query latency on an M4 Pro with 24 GiB RAM, using folder-local indexes:

| Dataset | Search | Before substring change | Current | fsearch |
| --- | --- | ---: | ---: | ---: |
| Linux | Filename | 0.0594 ms | 0.0584 ms | 0.1251 ms |
| Linux | Content | 1.0061 ms | 1.0499 ms | 2.3338 ms |
| Chromium | Filename | 0.1358 ms | 0.1457 ms | 0.3578 ms |
| Chromium | Content | 3.6940 ms | 3.7227 ms | 6.9376 ms |

Linux contains 95,939 visible files; Chromium contains 507,057.
Each engine runs separately for five rounds over a seeded workload.
Each round contains 300 exact filenames, 1,200 typos, and 67 literal content patterns.
Times include Unix socket round trips, with 50 results and no content-search time budget.
These are warm-workload medians, not cold-cache or whole-disk measurements.
The baseline uses the pre-change matching implementation with the same build dependencies.

Benchmark the normal fplussearch executable against either source tree:

```sh
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux --binary build/fplussearch --out benchmarks/linux.json
python3 benchmarks/benchmark.py ~/fplussearch-bench/chromium --binary build/fplussearch --out benchmarks/chromium.json
```

See the [benchmark harness guide](benchmarks/README.md) for setup, baseline capture, workload replay, and fsearch compatibility.
Raw benchmark JSON files remain local and aren't tracked by Git.
Timing differences vary between runs and don't establish a consistent speed improvement from the substring change.

Index pages are clean file-backed memory, so macOS can drop them under
memory pressure and read them back from the cache files.

## How it works

- Indexing uses `getattrlistbulk(2)` from 8 threads, which returns names,
  types and sizes for a whole directory in a few system calls. Names are
  deduplicated while scanning, and the build buffers are mapped in fixed
  chunks so they never get copied and are returned to the OS when freed.
- Each distinct name is stored once: 6.35 million entries share 1.44 million
  names. Folders and files have separate name sets, sorted by name, and
  entries are numbered so that all entries with the same name are
  contiguous. A matching name maps directly to its range of entries.
- Names made only of 16 or more lowercase hex digits (content-addressed
  caches such as the pnpm and npm stores and git objects) are packed two
  digits per byte in their own section. A query containing any other
  character skips that section; hex-only queries search the packed bytes
  directly at both nibble alignments.
- NEON filters character masks in batches before scoring candidate names.
  Name scoring uses case-insensitive substrings and one-edit typo matching.
  Work is split across search threads.
- Folders come first in the numbering, so a parent fits in 20 bits. Sizes
  are stored only for files, as 32 bits plus a small table for files of
  4 GB or more. Each file has a kind byte holding its type and a size class;
  `type:` and `size:` filters test 16 kind bytes at a time and read exact
  sizes only near a size boundary.
- Search scores distinct names, then ranks their entries using location,
  recency, and visibility adjustments. Score bounds let it skip candidates
  that cannot reach the top results while still counting matches.
- A refresh rebuilds the index from the previous one: folders FSEvents
  reported (and folders new to the index) are listed from disk, everything
  else is copied from the cached index, a whole unchanged subtree at a time.
  The cache records the FSEvents id it is current to, so the next launch
  replays only what happened since.
- Search threads run at interactive priority and spin for 250 ms after each
  search, because an idle core cluster takes milliseconds to ramp back up.
- Symbols come from a per-language token scanner (comments and strings are
  skipped) with rules for definitions in C, C++, Objective-C, Rust, Go,
  Python, JavaScript, TypeScript, Swift, Java, Kotlin, C#, Ruby, PHP, Lua,
  Zig and shell. The symbol index stores each distinct name once, with the
  files that define it; the line number is found again when a result is
  shown. Source files are read with the file cache bypassed, so indexing
  doesn't evict other cached files.

## Limitations

- Case-insensitive matching covers ASCII only. Names are matched byte for
  byte, so an accented character typed in a different Unicode normalization
  form than the file name will not match.
- Query mode (`fplussearch QUERY`) and `--bench` use the cache as it is;
  only the interactive mode refreshes it. While the interactive mode is open,
  changes can take up to 30 s to appear.
- Filename results are ranked by match quality, location, recency, and visibility.
  Substring matching doesn't support scattered-letter abbreviations.
- Symbol extraction is heuristic, not a parser: unusual code can be missed or
  misread. Source files over 1 MB and files that look minified are skipped,
  and only a name's first definition in each file is listed.

## Verify

```sh
make
make test-search
swift build -Xswiftc -warnings-as-errors
swift build -c release -Xswiftc -warnings-as-errors
python3 tests/test_daemon_disconnect.py
brew style Formula/fplussearch.rb
```

`make test-search` checks substring matching, letter edits beside digits, and rejection of digit edits.
The Python regression check uses an isolated temporary index and daemon.
It verifies that disconnecting during a large response doesn't stop the daemon.
It requires Python 3 and a built `build/fplussearch` executable.

## License

MIT. See [LICENSE](LICENSE).
