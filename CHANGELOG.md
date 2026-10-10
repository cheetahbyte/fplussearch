# Changelog

## Unreleased

- Organize C++ sources by subsystem and split scanning, matching, content indexing, live updates, protocol handling, and storage into focused files. Use `-Isrc` and subsystem-qualified headers for C++ embedding, including `live/live.hpp`. Update normal and profiling builds for nested sources and compiler-generated dependencies; retain existing matching checks without including `.cpp` files.

- Remove the interactive terminal interface. Running without a query now shows help and exits. Keep the CLI, daemon, C++ library, and Swift client.

- Replace long-name scalar substring tails with bounded overlapping NEON blocks. Reversed-order Chromium socket medians improve 7-8% and p90 about 19%; Linux socket medians remain unchanged. Add randomized NEON/scalar differential checks and guard-page coverage to `make test-search`. See `benchmarks/profiling/find_ci.md` for measurements and workload-specific regressions.

- Inline ordinary filename counts and keep overflow lookup in a cold helper, preserving the index format and fallback behavior. Reversed-order socket benchmarks improve Chromium filename medians by 2.2-3.6%; Linux socket results are inconclusive. See `benchmarks/README.md#inline-name-counts`.

- Consolidate benchmark reports into `benchmarks/README.md`, organize raw results under `benchmarks/results/`, and update runner output defaults and documentation links.

- Replace ancestor walks with query-local subtree bitmaps for global negation-only searches. Reversed-order socket benchmarks improve this query class by 29-31% on Linux and 48-49% on Chromium; ordinary filename latency remains inconclusive. See `benchmarks/README.md#ancestor-exclusion-bitmaps` for details.
- Add classified engine workloads and a three-round latency tool with p50/p90, CPU, peak RSS, and ordered-result captures.

- Accelerate bounded case-insensitive substring matching with first/last-byte filtering and ARM NEON checks. Repeated Chromium filename benchmarks improve median latency by 3-6% and p90 by about 15%; Linux results are inconclusive. See `benchmarks/README.md#bounded-substring-matching` for generic workloads and limitations.

- Filter filename candidates with immutable index-owned bit planes, preserving raw masks and matching semantics. Repeated three-round socket benchmarks reduce filename medians by 6-11% on Linux and 17-18% on Chromium; see `benchmarks/README.md#bit-sliced-candidate-masks`.

- Reduce ranked filename-search jobs from 24 to eight per worker; three-round Linux measurements show lower median latency, with Chromium validation pending.

- Release temporary filename-index buffers at their last use instead of retaining vector capacity.
- Parse each source file once per symbol response, preserving hit order and first-definition locations.
- Traverse small `in:` subtrees directly while retaining global scans for broad scopes.
- Share live-overlay bitmap pages and map blocks between snapshots, copying only modified blocks.
- Merge sorted content-document streams during synchronization instead of sorting a combined list.
- Encode content postings from spans and reuse insertion offsets to reduce temporary buffer overlap.

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
