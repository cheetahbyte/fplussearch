# fplussearch

Whole-disk file name search for macOS in C++. Every keystroke searches every
file and folder on the disk.

Inspired by [Noah's Rust file search engine](https://x.com/itsnoahd/status/2107993727809855570).

The code in this repository is entirely AI-generated.

## Build

```sh
make            # produces build/fplussearch (needs a C++20 compiler)
```

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

The first run indexes the disk (about 25 s for 6 million entries) and caches
the index in `~/Library/Caches/fplussearch/`. Later runs map the cache in a few
milliseconds and refresh it in the background at low priority.

Give your terminal Full Disk Access to index protected folders such as Mail
and Messages. Other volumes (`/Volumes`) are not indexed.

## Query syntax

All parts must match. Name matching is a case-insensitive substring match.

| Query | Matches |
| --- | --- |
| `report 2024` | Names containing both `report` and `2024` |
| `"my file"` | Names containing `my file` |
| `size:>1gb`, `size:<=10mb`, `size:500k` | File size (`>`, `>=`, `<`, `<=`, `=`; bare means `>=`; units b, k, m, g, t, base 1024) |
| `type:video` | `video`, `audio`, `image`, `doc`, `archive`, `code` by extension, plus `dir` and `file`; comma-separate for any of several |
| `ext:mp4,mov` | Names ending in any of the extensions |

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

The pause column varies between runs because it measures how quickly macOS
wakes idle cores.

Memory:

| | |
| --- | ---: |
| Index (memory-mapped cache file) | 96 MB |
| Process footprint while searching | 2.5 MB |
| Resident, including the mapped index | 99 MB |
| Peak while indexing | 282 MB |

The index pages are clean file-backed memory, so macOS can drop them under
memory pressure and read them back from the cache file.

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
- Search threads run at interactive priority and spin for 250 ms after each
  search, because an idle core cluster takes milliseconds to ramp back up.

## Limitations

- Case-insensitive matching covers ASCII only. Names are matched byte for
  byte, so an accented character typed in a different Unicode normalization
  form than the file name will not match.
- The index is a snapshot that is refreshed on each interactive launch;
  changes made while fplussearch is open do not appear until the next launch.
- Results list folders first, then files, each sorted by name (byte order);
  they are not ranked by relevance.

## License

MIT. See [LICENSE](LICENSE).
