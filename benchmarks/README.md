# Benchmark & Profiling Guide

This document consolidates instructions for running benchmarks and profiling `fplussearch`, the tooling workflow, verified performance optimizations, CPU profiling findings, and historical experiment outcomes.

---

## Table of Contents

1. [Running Benchmarks](#running-benchmarks)
   - [Basic Benchmark Runner (`benchmark.py`)](#basic-benchmark-runner-benchmarkpy)
   - [Repeating Workloads (`--workload`)](#repeating-workloads---workload)
   - [Compatibility with upstream `fsearch`](#compatibility-with-upstream-fsearch)
   - [Measurement Rules & Limits](#measurement-rules--limits)
2. [Classified Workloads & Engine Benchmarking](#classified-workloads--engine-benchmarking)
3. [Profiling Tools & Workflow](#profiling-tools--workflow)
   - [Build with Symbols](#build-with-symbols)
   - [Profile Daemon over Socket](#profile-daemon-over-socket)
   - [Profile Engine Directly](#profile-engine-directly)
   - [Export Stacks & Render Flamegraphs](#export-stacks--render-flamegraphs)
   - [Flamegraph Directory Index](#flamegraph-directory-index)
4. [Verified Optimizations & Results](#verified-optimizations--results)
   - [Bit-Sliced Candidate Masks](#bit-sliced-candidate-masks)
   - [Bounded Substring Matching](#bounded-substring-matching)
   - [Ancestor-Exclusion Bitmaps for Negation Queries](#ancestor-exclusion-bitmaps)
   - [Six Core Architectural & Indexing Optimizations](#six-core-architectural--indexing-optimizations)
   - [Worker Pool Task Granularity](#worker-pool-task-granularity)
5. [CPU Profiles & Bottleneck Analysis](#cpu-profiles--bottleneck-analysis)
6. [Rejected Experiments Log](#rejected-experiments-log)

---

## Running Benchmarks

### Basic Benchmark Runner (`benchmark.py`)

[benchmark.py](benchmark.py) measures filename and content query latency using the standard `fplussearch` executable over a local Unix domain socket. It does not build external tools, compile Rust, or alter existing system daemons.

**Prerequisites:** macOS on Apple Silicon, Python 3.9+, and `fd` on `PATH`.

Build `fplussearch` and run:

```sh
make -j8
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary build/fplussearch \
  --rounds 5 \
  --out benchmarks/linux.json

python3 benchmarks/benchmark.py ~/fplussearch-bench/chromium \
  --binary build/fplussearch \
  --rounds 5 \
  --out benchmarks/chromium.json
```

- Any source tree with at least 300 eligible unique filenames and enough source identifiers can be used.
- The runner reports two medians in milliseconds: **filename** and **content**. Lower is better.
- JSON output includes p50, p90, sample counts, exact-name and typo top-1 accuracy, executable hash, and the full replayable workload.

### Repeating Workloads (`--workload`)

To perform exact A/B comparisons without query variation:

1. Capture a baseline workload:
   ```sh
   make -j8
   cp build/fplussearch /tmp/fplussearch-before
   python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
     --binary /tmp/fplussearch-before \
     --rounds 3 \
     --out benchmarks/linux-before.json
   ```

2. After making changes and rebuilding, replay the saved workload:
   ```sh
   make -j8
   python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
     --binary build/fplussearch \
     --rounds 3 \
     --workload benchmarks/linux-before.json \
     --out benchmarks/linux-after.json
   ```

Replaying requires the same source folder path, guarantees identical query sequences, and does not require `fd`.

### Compatibility with upstream `fsearch`

The workload follows `fsearch/demo/vs_fff.py`:
- Random seed 1.
- 300 unique ASCII filenames and up to 1,200 letter typos.
- 60 sampled source identifiers and 7 fixed content patterns.
- Filename requests use `q` and `limit: 50`. Content requests use `op: grep`, literal mode, 50 results, 1 match per file, and no time budget.
- Output JSON exposes identical fields: `name_ms`, `grep_ms`, `exact_top1`, and `typo_top1`.

The older multi-engine comparison runner remains available at [compare.py](compare.py).

### Measurement Rules & Limits

- **Timing scope:** Timing includes Unix socket round trips, JSON serialization, and client parsing. Engine-level execution alone is significantly faster.
- **Rounds limit:** Invocations should remain at **3 rounds or fewer** for candidate evaluations to avoid excessive cache effects.
- **Run order:** Always run alternating and reversed-order comparisons (e.g. baseline then updated, followed by updated then baseline) to verify gains are not artifacts of thermal state or OS caching.
- **Daemon isolation:** The runner starts a temporary daemon with `--root` in a temporary home directory. It does not stop background daemons or flush the OS disk cache.

---

## Classified Workloads & Engine Benchmarking

For deeper inspection into specific query types without socket overhead, use the classified workload tools:

- Workload generator: `benchmarks/profiling/workload.py` generates queries split into categories (short queries, exact, typos, multi-token, negation-only, etc.).
- Engine measurement tool: `build/profiling/measure` tests `Engine::search` directly against an immutable index file, capturing p50/p90, total CPU time, peak resident memory, and ordered response digests.

---

## Profiling Tools & Workflow

### Build with Symbols

To profile without compromising optimizations (`-O3 -mcpu=native -g`):

```sh
make -f benchmarks/profiling/Makefile -j8
```

This builds `build/profiling/fplussearch`, `build/profiling/replay`, `build/profiling/measure`, and their debug symbol bundles (`.dSYM`).

### Profile Daemon over Socket

Profile an isolated daemon replaying a saved benchmark workload:

```sh
python3 -B benchmarks/profiling/socket_replay.py \
  build/profiling/fplussearch WORKLOAD.json /tmp/fplussearch-profile
```

This records separate 12-second traces for filename and content queries while the daemon is actively serving requests.

### Profile Engine Directly

Profile continuous in-memory search without socket or JSON overhead:

```sh
xctrace record --template 'Time Profiler' --time-limit 15s \
  --output /tmp/engine.trace --no-prompt --launch -- \
  build/profiling/replay INDEX.bin QUERIES.txt 8 12
```

### Export Stacks & Render Flamegraphs

1. Export the trace from Xcode's schema:
   ```sh
   xctrace export --input /tmp/engine.trace \
     --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
     --output /tmp/engine.xml
   ```

2. Convert to folded stack format:
   ```sh
   python3 -B benchmarks/profiling/export_stacks.py \
     /tmp/engine.xml benchmarks/profiles/engine --start 1 --end 12
   ```
   This generates `all.folded`, `query.folded`, and `summary.json`.

3. Render an interactive SVG using [FlameGraph](https://github.com/brendangregg/FlameGraph):
   ```sh
   curl -fsSL https://raw.githubusercontent.com/brendangregg/FlameGraph/master/flamegraph.pl -o /tmp/flamegraph.pl
   perl /tmp/flamegraph.pl --countname nanoseconds --hash \
     benchmarks/profiles/engine/all.folded > benchmarks/profiles/engine/all.svg
   ```

### Flamegraph Directory Index

Saved captures in `benchmarks/profiles/`:
- `chromium-median-engine/`: Median-band filename queries on Chromium ([all.svg](profiles/chromium-median-engine/all.svg), [query.svg](profiles/chromium-median-engine/query.svg))
- `chromium-engine/`: Full filename query workload on Chromium ([all.svg](profiles/chromium-engine/all.svg), [query.svg](profiles/chromium-engine/query.svg))
- `linux-daemon-filename/`: Daemon filename queries on Linux ([all.svg](profiles/linux-daemon-filename/all.svg), [query.svg](profiles/linux-daemon-filename/query.svg))
- `linux-daemon-content/`: Daemon content queries on Linux ([all.svg](profiles/linux-daemon-content/all.svg), [query.svg](profiles/linux-daemon-content/query.svg))
- `bitplanes-linux-filename/`, `bitplanes-linux-content/`, `bitplanes-chromium-filename/`, `bitplanes-chromium-content/`: Post-bitplane profiles.

---

## Verified Optimizations & Results

### Bit-Sliced Candidate Masks

Candidate mask evaluation originally scanned 64-bit masks linearly per name. The engine replaces this with **index-owned bit planes**:
- Each section stores 64 planes of 64 bits per block of up to 64 names.
- Candidate evaluation computes required-bit intersections and start-bit unions across planes using SIMD bitwise operations, identifying names missing 0 or 1 bit for typo acceptance.
- Disk format and matching semantics (`Token::fits`) remain unchanged.

**Socket Benchmark Results (3-round replays):**

| Dataset | Baseline Median | Updated Median | Delta (Forward) | Repeated Baseline | Repeated Updated | Delta (Reversed) |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| **Linux** | 70.7 µs | 66.4 µs | **-6.1%** | 72.3 µs | 64.6 µs | **-10.7%** |
| **Chromium** | 126.1 µs | 103.8 µs | **-17.7%** | 126.6 µs | 104.5 µs | **-17.4%** |

### Bounded Substring Matching

Filename scoring heavily relies on case-insensitive substring searching (`find_ci`).
- Vectorized endpoint prefiltering: checks both the **first and last query bytes** simultaneously across 16 positions using ARM NEON before verifying survivors in increasing position order.
- Loads are strictly bounded to the supplied string view (no out-of-bounds padding reads).
- Scalar fallback also applies endpoint filtering.

**Socket Benchmark Results (3-round replays):**

| Dataset | Baseline Median | Updated Median | Baseline p90 | Updated p90 | p90 Reduction |
| --- | ---: | ---: | ---: | ---: | ---: |
| **Linux** | 64.8 µs | 66.5 µs (+2.6%) | 80.4 µs | 81.5 µs | flat |
| **Linux (Reversed)** | 68.6 µs | 67.0 µs (-2.4%) | 81.5 µs | 80.0 µs | -1.8% |
| **Chromium** | 106.4 µs | 100.5 µs (**-5.5%**) | 284.5 µs | 239.8 µs | **-15.7%** |
| **Chromium (Reversed)** | 104.0 µs | 100.8 µs (**-3.1%**) | 279.8 µs | 237.0 µs | **-15.3%** |

Microbenchmark comparisons on synthetic patterns show dramatic reduction on difficult inputs (e.g. repeated first-character needles drop from 868 ns to 14.7 ns per call; misses drop from 85 ns to 19 ns).

### Ancestor-Exclusion Bitmaps

For global queries with negative tokens and no positive tokens (e.g. `!test`, `type:file !test`, `ext:cpp !third_party`):
- Instead of repeatedly walking parent pointers up directory chains for each candidate, the engine builds a query-local **ancestor-exclusion bitmap** in directory preorder.
- A single bit lookup per candidate directory replaces parent-chain traversals.

**Results:**
- **Linux socket median:** 29–31% faster on negation-only queries.
- **Chromium socket median:** 48–49% faster on negation-only queries.
- **Full-disk engine latency:** ~75% reduction on negation-only query classes.

### Six Core Architectural & Indexing Optimizations

1. **Buffer release:** Scratch buffers in filename indexing are released at last use using empty-vector swap instead of empty initializer assignment (which retained capacity).
2. **Symbol batching:** Symbol hits are grouped by file and parsed once per response snapshot rather than repeatedly per match.
3. **Narrow `in:` scopes:** Traverses narrow subtrees directly if candidate limit is < 16,384 entries or < 1/16th of the index.
4. **Copy-on-write overlay:** Deletion-bitmap pages and sorted overlay blocks are shared between snapshots, copying only modified pages.
5. **Sorted stream merge:** Content synchronization merges sorted segment document streams directly, eliminating redundant combined document sorts.
6. **Zero-copy posting spans:** Posting spans pass directly to the segment writer, eliminating intermediate copies and auxiliary offset arrays.

### Worker Pool Task Granularity

Reducing `Engine::search` task splitting from 24 jobs per worker to **8 jobs per worker** lowered coordination and container merging overhead, reducing Linux filename socket median from ~0.070 ms to ~0.052 ms (~24% reduction).

---

## CPU Profiles & Bottleneck Analysis

Captures profiling the engine after bit-plane filtering show the following distribution:

| Dataset / Workload | Search Median | Primary Hotspot | Profile Evidence |
| --- | ---: | --- | --- |
| **Linux Filename** | ~0.060 ms | Candidate-mask filtering & fuzzy scoring | `fit_bits` ~34.8% of query stack; name matching ~25.2%. |
| **Chromium Filename** | ~0.103 ms | Substring & fuzzy scoring | `name_hit` ~57.0%; `token_score` ~51.8%; `fuzzy_capped` ~45.0%. |
| **Linux Content** | ~1.12 ms | File access during verification | `read_file` 91.8% (`__open` 80.5%, `read` 7.2%). |
| **Chromium Content** | ~4.39 ms | File access during verification | `read_file` 88.8% (`__open` 79.2%, `read` 5.9%). |

- In content search, trigram posting list intersection consumes <0.1% of CPU. Fresh filesystem file verification dominates content query latency.
- In filename search on Linux daemons, scheduler yields (`swtch_pri`) account for ~62% of whole-process CPU during inter-query socket pauses, which is expected behavior for keeping performance-core clusters warm.

---

## Rejected Experiments Log

To prevent re-investigating dead ends, the following ideas were evaluated and rejected due to lack of repeatable gains or regressions:

| Trial | Rationale / Result | Decision |
| --- | --- | --- |
| **Query filter-plan precomputation** | Building precomputed bit-plane indices and branching exact intersection per token produced no repeatable latency gain (<1.5% noise that reversed on second run). | **Rejected** |
| **Single-token NEON mask filtering** | Initial Chromium improvement vanished when reversing benchmark variant run order. | **Rejected** |
| **Eager prefix ranking / lazy sort** | Sorting candidates lazily produced inconsistent latencies and worse Chromium p90. | **Rejected** |
| **Progressive prefix expansion** | Better medians in one run, but regressed Chromium average and p90. | **Rejected** |
| **Reducing filename jobs to 4 per worker** | Worse Chromium tail latency. 8 jobs per worker proved optimal. | **Rejected** |
| **Incremental chunk reading (64 KiB)** | Reading content files in small chunks improved mean slightly but did not lower medians. | **Rejected** |
| **Geometric buffer growth (8 KiB start)** | Regressed Chromium content average latency. | **Rejected** |
| **Selective single-threaded search on caller** | Large Chromium regression due to lost multi-core parallelism. | **Rejected** |
| **Reducing content reader threads (to 4 or 8)** | Inconsistent medians and worse tail latency. | **Rejected** |
| **Typo score bound pruning skips** | Skipping typo scorers when bounds cannot beat current minimum had inconsistent gains across run orders. | **Rejected** |
| **Worker counter cacheline separation** | Separating worker atomic counters onto individual cachelines produced inconsistent Chromium results. | **Rejected** |
| **Claiming 4 jobs at a time in pool** | Median filename latency regressed on both Linux and Chromium. | **Rejected** |
