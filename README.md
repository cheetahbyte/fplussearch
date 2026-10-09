# fplussearch

Whole-disk filename and code symbol search for macOS, written in C++.
Search files, folders, functions, and types with every keystroke.

Inspired by [Noah's Rust file search engine](https://github.com/noahdunnagan/fsearch).

The code in this repository is entirely AI-generated.

## Install with Homebrew

Requires macOS on Apple Silicon. This repository includes a Homebrew tap.

```sh
brew tap cheetahbyte/fplussearch https://github.com/cheetahbyte/fplussearch.git
brew install --HEAD cheetahbyte/fplussearch/fplussearch
```

The formula builds from `main`; no tagged release is available yet.
The tap requires the formula to be published to this repository.

Swift clients start the shared daemon automatically. To start it at login:

```sh
brew services start cheetahbyte/fplussearch/fplussearch
```

Don't use `sudo` or also run `fplussearch install --login` (a separate binary and login item).
Stop any running daemon before enabling the service. To stop the service:

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

Add this repository as a Swift package dependency and select `FPlusSearch`.
It connects through `fplussearch stdio`; install the executable separately with Homebrew or `make`.
See [Swift integration](docs/swift.md) for configuration, search, lifecycle, and sandbox limitations.

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

The first run caches the index in `~/Library/Caches/fplussearch/`: about 25 s
for 6 million entries, plus 45 s for symbols in 2 million source files.
Later runs map the cache in milliseconds. Interactive mode uses FSEvents to
refresh changed folders and source files in the background (about 2 s for the
whole disk), then checks every 30 s. It fully rescans only if FSEvents loses track of changes.

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

Linux: 95,939 visible files; Chromium: 507,057. Each engine runs five seeded
rounds of 300 exact filenames, 1,200 typos, and 67 literal content patterns.
Warm-workload medians include socket round trips, with 50 results and no content-search time budget.
They don't measure cold-cache or whole-disk performance. The baseline uses pre-change matching with the same dependencies.

Benchmark the normal fplussearch executable against either source tree:

```sh
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux --binary build/fplussearch --out benchmarks/linux.json
python3 benchmarks/benchmark.py ~/fplussearch-bench/chromium --binary build/fplussearch --out benchmarks/chromium.json
```

See the [benchmark harness guide](benchmarks/README.md) for setup and comparison details.
Raw JSON stays local and untracked. Run-to-run variation doesn't establish a consistent speed improvement from the substring change.

Index pages are clean file-backed memory, so macOS can drop them under
memory pressure and read them back from the cache files.

## How it works

- Eight indexing threads use `getattrlistbulk(2)` to read directory metadata in batches.
  Names are deduplicated; mapped build buffers avoid copying.
- Each name is stored once: 6.35 million entries share 1.44 million names.
  Sorted name sets map matches directly to contiguous entry ranges.
  Long lowercase hex names use a separate packed section, skipped by non-hex queries.
- NEON filters candidate names in batches before parallel substring and typo scoring.
  Compact parent, size, and type fields support batched metadata filters.
- Results rank by match quality, location, recency, and visibility.
  Score bounds skip candidates that can't reach the top results while still counting matches.
- FSEvents refreshes changed folders and copies unchanged subtrees from the cache.
  The saved event ID lets later launches replay only new changes.
- Interactive-priority search threads spin for 250 ms after searches to avoid idle-core startup delays.
- Per-language token scanners extract symbols from C, C++, Objective-C, Rust, Go,
  Python, JavaScript, TypeScript, Swift, Java, Kotlin, C#, Ruby, PHP, Lua, Zig, and shell.
  Symbol names are deduplicated; line numbers are resolved when displayed.
  Source reads bypass the file cache to avoid evicting other cached files.

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

`make test-search` checks substring and typo matching, including digit preservation.
The Python check verifies daemon survival after a mid-response disconnect using an isolated temporary index.
It requires Python 3 and `build/fplussearch`.

## License

MIT. See [LICENSE](LICENSE).
