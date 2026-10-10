# Bounded substring matching

## Decision

Retain first/last-byte filtering in the case-insensitive substring matcher used by filename scoring.
Chromium filename median, p90, and mean improve in both three-round comparisons.
Linux filename results are inconclusive: small median and mean changes reverse direction across comparisons.
Generic matcher workloads also improve, except for effectively flat short-string and near-full-query cases.

This is an algorithmic change, not a Linux- or Chromium-specific path.
No corpus-derived threshold, index data, heap allocation, or dependency is added.
These results do not establish improvements on every corpus or processor.

## Implementation

`find_ci` checks the first and last query bytes at each candidate position before comparing the full substring.
On ARM NEON, it checks 16 positions simultaneously and verifies survivors in increasing position order.
Every vector load remains within the supplied string view; it does not rely on index padding.
Single-byte queries use the existing byte scanner. Two-byte queries need no further verification after both endpoint checks.
The scalar fallback applies the same endpoint filtering.

The existing padded-blob matcher already uses this technique. The change applies it to bounded views used during scoring and name acceptance.
Original filename bytes, case-folding rules, earliest-match positions, scoring, and typo behavior remain intact.
Duplicate-search elimination is not part of this change.

## Socket benchmarks

All timings come from isolated daemons on Apple Silicon. Linux and Chromium refer to source-tree datasets, not host operating systems.
Each invocation runs three rounds: 4,500 filename requests and 201 content requests.
The saved workloads match the earlier bit-plane comparisons.
Baseline executables contain the accepted bit-plane optimization, without this substring change.

| Dataset | Baseline filename median | Changed median | Repeated baseline | Repeated changed |
| --- | ---: | ---: | ---: | ---: |
| Linux | 64.792 µs | 66.500 µs | 68.625 µs | 67.000 µs |
| Chromium | 106.355 µs | 100.541 µs | 104.021 µs | 100.750 µs |

Linux is 2.6% slower initially and 2.4% faster in the reversed-order comparison.
Chromium is 5.5% faster initially and 3.1% faster in the reversed-order comparison.

| Dataset | Baseline filename p90 | Changed p90 | Repeated baseline | Repeated changed |
| --- | ---: | ---: | ---: | ---: |
| Linux | 80.375 µs | 81.541 µs | 81.542 µs | 80.041 µs |
| Chromium | 284.542 µs | 239.834 µs | 279.791 µs | 237.000 µs |

Chromium filename p90 improves 15.7% and 15.3%; mean improves 10.5% and 9.3%.
Linux mean changes from 62.002 to 63.134 µs initially, and 64.311 to 62.910 µs in the repeat.

Content medians:

| Dataset | Baseline | Changed | Repeated baseline | Repeated changed |
| --- | ---: | ---: | ---: | ---: |
| Linux | 1.3312 ms | 1.3222 ms | 1.3143 ms | 1.3212 ms |
| Chromium | 3.2120 ms | 3.3244 ms | 3.2835 ms | 3.2943 ms |

Chromium content medians are slightly slower in both comparisons; repeated p90 increases from 9.3868 to 10.6624 ms.
No content speedup is claimed. Content read and matching algorithms are unchanged.

Initial order: baseline Linux, baseline Chromium, changed Linux, changed Chromium.
Repeated order: changed Chromium, changed Linux, baseline Chromium, baseline Linux.
Raw measurements are ignored files named `benchmarks/substring-pair-{linux,chromium}-{before,after,before-repeat,after-repeat}.json`.
Each JSON includes the workload, executable hash, and accuracy statistics.

## Generic matcher workloads

A temporary microbenchmark compares the original matcher with the changed production matcher.
It generates 2,048 cases per group using seed 41, with 300 passes per timing and three alternating variant measurements.
Query bytes use the existing fold table. Tests compare returned positions before measuring.
These isolated function timings are not end-to-end search latency.

| Workload | Original median per call | Changed median per call |
| --- | ---: | ---: |
| Short strings, 1-15 bytes | 6.40 ns | 6.33 ns |
| Medium strings, 16-47 bytes | 12.93 ns | 10.11 ns |
| Long strings, 64-256 bytes | 47.98 ns | 13.21 ns |
| Repeated first-character matches | 868.40 ns | 14.66 ns |
| Late matches | 87.43 ns | 17.58 ns |
| No matches | 84.97 ns | 19.26 ns |
| Arbitrary byte strings | 11.99 ns | 11.16 ns |
| Queries almost as long as the string | 57.77 ns | 58.17 ns |

Groups and seed were fixed before observing their timings. No implementation tuning followed these measurements.
The short and near-full-query differences are too small to establish improvement or regression.
Synthetic function workloads support generality, but do not replace held-out real-corpus benchmarks.

## Verification

- SIMD and forced scalar paths each pass 300,000 comparisons against a scalar reference under AddressSanitizer and UndefinedBehaviorSanitizer.
- Checks cover mixed case, punctuation, arbitrary bytes, repeated characters, misses, empty queries, and string lengths from 0 to 512.
- Guard pages immediately before or after each supplied view detect out-of-bounds loads.
- All 1,500 complete filename responses match per dataset, excluding timing.
- All 67 content results match per dataset, including candidate counts and completion flags.
- Diagnostic file-read counts differ for 18 Linux and 34 Chromium requests; complete response parity is not claimed for that field.
- Exact-name and typo top-result accuracy remain unchanged across timed runs.
- Existing C++ matching checks, Swift debug and release builds with warnings as errors, daemon disconnect checks, Homebrew style, and whitespace checks pass.

The correctness and microbenchmark programs remain temporary validation tools, not additions to the default test suite.
Scalar correctness was checked on Apple Silicon by disabling NEON at compilation. Performance on another architecture was not measured.
