# Index-owned bit-sliced masks

## Result

Filename socket medians improve on both datasets in repeated three-round runs. Retain the index-owned bit-plane implementation.

| Dataset | Baseline median | Changed median | Reduction | Repeated baseline | Repeated changed | Repeated reduction |
| --- | ---: | ---: | ---: | ---: | ---: | ---: |
| Linux | 70.708 µs | 66.375 µs | 6.1% | 72.333 µs | 64.583 µs | 10.7% |
| Chromium | 126.146 µs | 103.833 µs | 17.7% | 126.563 µs | 104.542 µs | 17.4% |

Each invocation runs three rounds, with 4,500 filename requests and 201 content requests. Timing includes socket transport and Python response parsing.

Run order: baseline Linux, baseline Chromium, changed Linux, changed Chromium, baseline Chromium, baseline Linux, changed Chromium, changed Linux.
The middle changed-to-baseline sequence also validates the filename gain with reversed variant order.

Content medians:

| Dataset | Baseline | Changed | Repeated baseline | Repeated changed |
| --- | ---: | ---: | ---: | ---: |
| Linux | 1.3410 ms | 1.3380 ms | 1.2234 ms | 1.3510 ms |
| Chromium | 3.5130 ms | 3.2682 ms | 3.4318 ms | 3.1462 ms |

Linux content results are inconsistent, including a 10.4% regression in the repeated comparison. No content speedup is claimed.

## Implementation

Each section owns shared, immutable planes built from its original masks before index publication.
Build, cache load, refresh, and memory fallback all use the same attachment path.
Index copies share planes; refreshed indexes receive new planes without modifying retained snapshots.

Each block stores 64 planes of 64 bits. Candidate evaluation tracks names missing zero or exactly one query-mask bit.
Required-bit intersections and start-bit unions preserve `Token::fits` semantics. Tokens are combined by union, preserving ancestor matching.
Ranked search and `each_match` use the shared candidate scan. Raw masks, overlay scoring, and the disk format remain unchanged.

Planes add 512 bytes per block of up to 64 names, plus allocation metadata.
They are heap-backed, unlike the mapped raw index. Plane construction latency and daemon resident-memory changes were not measured.

## Verification

- A temporary check compares the production evaluator against `Token::fits` for 650,000 bitmaps, including every tail length from 0 to 64.
- Temporary lifecycle checks cover build, load, refresh, retained snapshots, memory fallback, ranked search, and the shared name scan.
- All 1,500 complete filename responses match per dataset after excluding timing.
- All 67 content results match per dataset, including paths, lines, candidate counts, and completion flags.
- Content diagnostic `read` counts differ for 15 Linux and 29 Chromium patterns; complete response parity is not claimed for that field.
- Exact-name and typo top-result accuracy remain unchanged across all timed runs.
- C++ matching checks, Swift debug and release builds with warnings as errors, daemon disconnect checks, and Homebrew style pass.

The additional checks are temporary validation tools, not additions to the default test suite.
Concurrent index replacement and overlay lifecycle were not separately stress-tested.

## Reproduction

Baseline source revision: `6d59614`. Both executables use the default optimized Makefile build on Apple Silicon.
Linux revision: `6c377d19d4a5116d9bec5203aa3c6c11523e7898`.
Chromium revision: `8b380f69cb2697188431ced311b16cd04e6a3c83`.

Workloads replay `standalone-linux.json` and `standalone-chromium.json`, without changing either source tree.
Raw results are ignored files named `benchmarks/bitplanes-{linux,chromium}-{before,after,before-repeat,after-repeat}.json`.
Each JSON includes the executable hash, workload, and statistics.

To replay a saved baseline workload:

```sh
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary build/fplussearch --rounds 3 \
  --workload benchmarks/bitplanes-linux-before.json \
  --out benchmarks/bitplanes-linux-after.json
python3 benchmarks/benchmark.py ~/fplussearch-bench/chromium \
  --binary build/fplussearch --rounds 3 \
  --workload benchmarks/bitplanes-chromium-before.json \
  --out benchmarks/bitplanes-chromium-after.json
```
