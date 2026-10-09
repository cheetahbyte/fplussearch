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

All parts must match. Name matching is a case-insensitive substring match.

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

On an M4 Pro with 6.35 million files and folders (`build/fplussearch --bench`):

| Query | Matches | While typing | First key after a pause |
| --- | ---: | ---: | ---: |
| `a` | 3,860,859 | 0.05 ms | 0.07 ms |
| `size:>1gb type:video` | 0 | 0.06 ms | 0.5 ms |
| `type:image` | 97,706 | 0.16 ms | 1.3 ms |
| `readme` | 36,933 | 0.25 ms | 0.7 to 2.6 ms |
| `node_modules` | 13,956 | 0.22 ms | 0.6 to 2.5 ms |
| `2024` | 2,427 | 0.32 ms | 1.1 to 2.4 ms |
| `sym:useEffect` | 985 | 0.24 ms | 2.8 ms |
| `sym:parse` | 309,109 | 0.43 ms | 3.7 ms |
| `sym:render ext:tsx` | 414 | 0.62 ms | 7.4 ms |

The pause column varies between runs because it measures how quickly macOS
wakes idle cores.

Memory:

| | |
| --- | ---: |
| File index (memory-mapped cache file) | 96 MB |
| Symbol index, 16.9 million definitions (memory-mapped, read only by `sym:` queries) | 128 MB |
| Process footprint while searching | 2.4 MB |
| Peak while indexing | 323 MB |

The index pages are clean file-backed memory, so macOS can drop them under
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
- A query scans the name bytes for its longest term with NEON and checks
  other terms per candidate name. Work is split across all cores in jobs of
  equal byte size.
- Folders come first in the numbering, so a parent fits in 20 bits. Sizes
  are stored only for files, as 32 bits plus a small table for files of
  4 GB or more. Each file has a kind byte holding its type and a size class;
  `type:` and `size:` filters test 16 kind bytes at a time and read exact
  sizes only near a size boundary.
- One- and two-character queries match most of the disk. The index stores
  how many entries contain each character and character pair, so these
  queries read the total from a table and only scan until the screen is
  full.
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
- Results list folders first, then files, each sorted by name (byte order);
  they are not ranked by relevance.
- Symbol extraction is heuristic, not a parser: unusual code can be missed or
  misread. Source files over 1 MB and files that look minified are skipped,
  and only a name's first definition in each file is listed.

## Verify

```sh
make
swift build -Xswiftc -warnings-as-errors
swift build -c release -Xswiftc -warnings-as-errors
python3 tests/test_daemon_disconnect.py
brew style Formula/fplussearch.rb
```

The Python regression check uses an isolated temporary index and daemon.
It verifies that disconnecting during a large response doesn't stop the daemon.
It requires Python 3 and a built `build/fplussearch` executable.

## License

MIT. See [LICENSE](LICENSE).
