# Bounded substring tail measurements

Keep the first/last-byte filter and replace its long-name scalar tail with one overlapping NEON block.
Leave needle setup, probe selection, index storage, and caller filtering unchanged.

## Sample attribution

The baseline is commit `16e0aedd6c23f9d45bc6c22a1f7cddb6bd6e8345`.
An isolated build uses `-O2 -g -mcpu=native`, without counters or extra instrumentation.
A 12-second Time Profiler capture replays the cached Chromium index with eight workers.
Analysis uses running leaf samples between seconds 1 and 10.
The executable and debug-symbol UUIDs match.

The analysis resolves XML frame references and removes the executable's address slide.
Exported frame addresses are aligned to four-byte ARM instruction boundaries before matching disassembly.
Region boundaries follow instructions, not source-line labels.

`find_ci` accounts for 29.0% of sampled engine-query CPU time in this capture.
Its own samples divide as follows:

| Region | Share of `find_ci` self samples |
| --- | ---: |
| Scalar tail | 51.3% |
| NEON probe loop and control | 15.2% |
| Setup and splats | 7.5% |
| Verification and candidate enumeration | 0.1% |
| Return and control | 22.2% |
| Entry and early control | 3.7% |

The single-byte path receives no samples in this workload.
These are approximate instruction-region counts, not exact execution costs.
Sampling skid can move weight between neighboring regions, especially into the shared return sequence.
The capture uses `/tmp/chromium-queries.txt`; the broader input-capture workload differs.
Its small verification share does not describe every query mix.

## Real input capture

A separate counting build searches each existing 2,412-query workload once, without warmup.
It uses one worker and a result limit of 100.
Single-worker execution keeps input sampling deterministic and avoids per-call synchronization.
Timed builds contain no counters.
Counting-run response rows match the uninstrumented baseline byte-for-byte on both datasets.

The capture records exact byte-length histograms, misses, verification attempts, and calls per query.
It also counts repeated name/needle byte pairs within each query.
These include names encountered through ancestor matching, not just filename candidates.
Repeated pairs can belong to different entries; the count does not establish that caching their complete scores is safe.

| Measurement | Linux | Chromium |
| --- | ---: | ---: |
| Calls | 4,622,230 | 113,155,850 |
| Mean calls per query | 1,916 | 46,914 |
| Names shorter than 16 bytes | 1,446,450 | 1,935,715 |
| Names of 16-31 bytes | 2,600,950 | 32,532,899 |
| Names of at least 32 bytes | 574,830 | 78,687,236 |
| Calls with at least 16 possible starts | 53.6% | 93.4% |
| Calls returning `npos` | 70.3% | 89.2% |
| Failed full verifications | 254,719 / 392,079 (65.0%) | 12,248,045 / 14,630,211 (83.7%) |
| Repeated pairs within a query | 4.7% | 2.2% |

Vector eligibility excludes empty needles and needles longer than the name; it includes single-byte needles.
Full verification counts cover needles of at least three bytes.
Two-byte probe hits already establish a complete match.

A fixed-seed SplitMix64 stream samples actual name/needle/result pairs with probability `1/1024` per call.
The replay contains 4,492 Linux pairs and 110,444 Chromium pairs.
Every replayed variant must return the captured first-match position.
The exact histograms and per-query counters remain separate from this sampled replay.

## Replay comparisons

The replay compares the original matcher with four temporary variants:

- Overlapping tail with per-call first/last setup.
- Precomputed first/last offsets, masks, and NEON splats.
- Precomputed probes selected using a fixed byte-rarity heuristic.
- Precomputed rare probes with an overlapping tail.

The heuristic ranks uncommon letters and digits ahead of common letters, dots, underscores, and hyphens.
It is not a measured filename-frequency table.
Precomputed matchers are built once per distinct needle before timing.
Each invocation makes approximately three million calls, in forward, reverse, then forward variant order.

| Variant | Linux mean time per call | Chromium mean time per call |
| --- | ---: | ---: |
| Baseline | 4.81 ns | 13.95 ns |
| Overlapping tail | 3.99 ns (-17.0%) | 7.56 ns (-45.8%) |
| Hoisted first/last setup | 4.87 ns (+1.1%) | 14.18 ns (+1.6%) |
| Hoisted rare probes | 4.82 ns (+0.2%) | 13.37 ns (-4.2%) |
| Rare probes and overlapping tail | 3.78 ns (-21.4%) | 7.11 ns (-49.0%) |

Repeated replay benefits from cache and branch-prediction locality.
Its absolute costs are not estimates of daemon costs.
Hoisting alone does not help this replay.
The rare-probe improvement is small and inconsistent across datasets, so only the tail advances to engine and socket measurements.

## Engine and socket comparisons

Uninstrumented timing builds use the normal `-O3 -g -mcpu=native` flags.
Each dataset runs baseline, tail, tail, then baseline, with three rounds per invocation.
Engine and socket runs use eight workers and a result limit of 50.
No profiling, counting, or other benchmark job overlaps the timed runs.
Socket invocations use isolated daemons, wait for content indexing, and warm the complete workload.

Negative changes mean lower latency.
Aggregate changes compare the mean invocation quantiles, rather than pooling invocations.

| Measurement | Linux change | Chromium change |
| --- | ---: | ---: |
| Engine overall median | +2.9% | -1.7% |
| Engine overall p90 | -3.7% | -14.7% |
| Socket overall median | +0.04% | -7.7% |
| Socket overall p90 | -2.4% | -19.0% |
| Socket multi-token median | -3.7% | -15.5% |
| Socket sampled long-name exact median | -3.2% | -18.8% |

Chromium socket overall median changes are -8.3% and -7.2% in the two comparison orders.
Its overall p90 changes are -19.3% and -18.8%.
Linux socket overall median changes are -0.09% and +0.17%, which do not establish a median improvement.
Linux engine medians regress in both orders, by 2.3% and 3.6%.
The short-query group's p90 regresses on Chromium despite its improved median.
The full report retains these group-level regressions; this is not a universal speedup.

All ordered engine response rows, scores, and totals match across variants.
Every timed socket response preserves ordered hits and totals, including scores in hits.

## Implementation and verification

For multi-byte needles with at least 16 possible starts, the final block begins at `last - 15`.
It loads only valid candidate positions and masks lanes already checked by preceding blocks.
The shift is at most 60 bits because the next unchecked start remains within that final block.
If preceding blocks consume every start, the matcher returns without shifting.
Names with fewer than 16 possible starts retain scalar scanning.
Single-byte matching and non-NEON builds remain unchanged.

`make test-search` includes direct differential tests for the private matcher, without adding a public testing API.
Each NEON and scalar run checks 421,450 cases against a naive byte-folding search.
Cases cover every match position across 1-65 possible starts, empty needles, single-byte needles, equal lengths, oversized needles, and non-ASCII bytes.
Guard pages surround the tested storage to catch reads outside the name.
A separate AddressSanitizer and UndefinedBehaviorSanitizer run checks the same matcher cases successfully.
The existing matching and daemon-disconnect checks also pass.
The sanitizer build instruments the matcher and test translation unit, not every linked engine object.

## Local evidence

Raw results and session-specific scripts are preserved in the ignored directory `benchmarks/results/find-ci-tail-experiment/`:

- `manifest.json` records build flags, workload hashes, and executable hashes.
- `summary.json`, `engine.json`, and `socket.json` retain invocation and group-level results.
- `*-counts.tsv` and `*-queries.tsv` retain exact histograms and per-query counters.
- `*.pairs` retain sampled inputs in a local binary format.
- `matcher.cpp`, baseline source snapshots, and `tail-search.cpp` retain the measured implementations.
- `profile/` retains region totals, annotated instructions, the capture manifest, and its analysis script.

The original trace and XML remain in `/tmp/find-ci-regions/`.
Scripts retain session-specific paths and expect the frozen baseline build; they are not a portable benchmark package.
Captured pairs contain local filename and directory-name bytes. Inspect them before sharing.
