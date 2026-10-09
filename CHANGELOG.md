# Changelog

## Unreleased

- Match contiguous, case-insensitive name substrings instead of subsequences, retaining one-edit typo tolerance and match-quality ranking.
- Allow letter insertions and deletions beside digits while keeping digit edits excluded from typo matching.
- Add `make test-search` for name-matching regression checks.

- Add a reproducible folder-local benchmark against fsearch for Linux and Chromium, with query latency, typo accuracy, and content coverage results.
- Add isolated baseline comparisons, repeated passes, result-parity checks, and memory observations; match fsearch's production allocator in the benchmark harness.
- Add a standalone benchmark for the normal fplussearch executable, using fsearch-compatible queries and socket requests with replayable workloads and separate filename/content medians.

- Reduce retained memory after indexing and searches by directly mapping large temporary buffers and releasing grep read buffers after each search.
- Reduce duplicate grep candidate storage and release oversized daemon request and response buffers after processing.

- Add the `FPlusSearch` Swift package for asynchronous access to the shared macOS daemon.
- Add a Homebrew formula in `Formula/fplussearch.rb`, making this repository an explicit-URL custom tap.
- Document Swift integration, the JSON-lines daemon protocol, and Homebrew installation and services.
- Declare Apple Silicon as the engine's supported architecture. Homebrew builds avoid host-specific CPU flags.
- Prevent client disconnects during large responses from killing the shared daemon with `SIGPIPE`.
