# Benchmarks

Run commands from the repository root. Requirements: macOS on Apple Silicon, Python 3.9+, and `fd` on `PATH`.

## Directory layout

- `benchmark.py`: standalone filename and content latency runner.
- `compare.py`, `common.py`, `fsearch-harness.rs`: comparison runner and shared tooling.
- `profiling/`: engine measurement and CPU profiling tools. See the [profiling guide](profiling/README.md).
- `results/`: raw JSON and replay workloads, ignored except for retained content experiment data.
- `profiles/`: ignored local CPU captures and flamegraphs.

## Run benchmarks

Build and measure a source tree:

```sh
make -j8
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary build/fplussearch --rounds 3 \
  --out benchmarks/results/linux.json
```

Use a tree with at least 300 eligible unique filenames and enough source identifiers.
The runner reports filename and content medians, p90, top-result accuracy, executable hashes, and replayable workloads.
Times include socket transport, JSON serialization, and Python response parsing.
The runner starts an isolated daemon; it does not stop existing daemons or flush the OS page cache.

## Repeating workloads (`--workload`)

Preserve the baseline executable and workload before changing the engine:

```sh
cp build/fplussearch /tmp/fplussearch-before
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary /tmp/fplussearch-before --rounds 3 \
  --out benchmarks/results/linux-before.json
```

After rebuilding, replay the same workload:

```sh
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary build/fplussearch --rounds 3 \
  --workload benchmarks/results/linux-before.json \
  --out benchmarks/results/linux-after.json
```

Replay requires the same source folder path but does not require `fd`.
Use at most three rounds per candidate invocation, repeat in reversed variant order, and check ordered results before accepting gains.
Diagnostic allocation probes and CPU profiles are not latency evidence.

## Compare with fsearch

`compare.py` builds a folder-local fsearch library harness. It requires a local fsearch checkout and Rust tooling.
See `python3 benchmarks/compare.py --help` for options.
The shared workload uses seed 1, 300 exact filenames, up to 1,200 typos, and 67 literal content patterns.
Requests return at most 50 results, with one content match per file and no content time budget.

Updated comparison on Apple M4 Pro, after pulling fsearch from `76d612f` to `d753915`:

fplussearch includes the uncommitted overlapping NEON tail change on top of `16e0aed`.
Each engine runs alone, with three rounds per invocation, first in forward order and then reversed order.
Ranges show the two invocation results, not confidence intervals.
Both datasets contain 1,500 filename queries and 67 content patterns.

| Dataset and metric | fplussearch | fsearch |
| --- | ---: | ---: |
| Linux filename median | 0.059-0.064 ms | 0.046-0.048 ms |
| Linux filename p90 | 0.091-0.094 ms | 0.061-0.073 ms |
| Chromium filename median | 0.094 ms | 0.081-0.090 ms |
| Chromium filename p90 | 0.207-0.208 ms | 0.148-0.186 ms |
| Linux content median | 1.139-1.154 ms | 0.767-0.814 ms |
| Linux content p90 | 6.350-7.454 ms | 1.499-1.606 ms |
| Chromium content median | 4.700-5.000 ms | 1.270-1.469 ms |
| Chromium content p90 | 27.658-27.957 ms | 2.422-2.488 ms |
| Linux typo top-result accuracy | 98.75% | 97.75% |
| Chromium typo top-result accuracy | 99.67% | 99.17% |

Exact filename top-result accuracy is 100% for both engines on both datasets.
Each engine's ordered filename and content response digests match between invocation orders.
This checks repeatability within each engine, not result equivalence between engines.

Content file counts differ for 21 of 67 Linux patterns and 59 of 67 Chromium patterns.
Content timing is therefore not an equal-recall comparison.
fplussearch runs its full daemon, watcher, and symbol index; fsearch runs a library harness without equivalent lifecycle work.
Startup and memory figures do not compare equivalent services.
The fsearch checkout remains clean after the update and benchmark.

Raw measurements: `results/fsearch-updated-{forward,reverse}.json` and `results/fsearch-updated-summary.json`.
The summary records the fplussearch executable hash because its working-tree optimization is not represented by the commit alone.
Historical measurements remain in `results/chromium-vs-fsearch-current.json` when available locally.
These are workload-specific results, not whole-disk guarantees.

## Retained optimizations

### Bit-sliced candidate masks

Immutable index-owned bit planes replace linear candidate-mask scans while preserving raw masks, matching semantics, and disk format.
Repeated three-round socket comparisons reduce filename medians by 6-11% on Linux and 17-18% on Chromium.
Planes add 512 bytes per block of up to 64 names, plus allocation metadata.
Construction latency and whole-daemon memory impact were not measured. No content speedup is established.

### Bounded substring matching

First/last-byte filtering uses bounded ARM NEON loads with a scalar fallback.
Repeated Chromium comparisons reduce filename median latency by 3-6% and p90 by about 15%.
Linux results are inconclusive. Synthetic matcher speedups do not establish equivalent end-to-end gains.

### Inline name counts

`Section::count` reads ordinary byte counts inline; a cold, non-inlined helper resolves overflow counts.
The index format and the missing-overflow fallback remain unchanged.
Only 4 of 77,334 Linux names and 41 of 420,494 Chromium names require overflow lookup in the measured indexes.
These are name frequencies, not dynamic call frequencies.

Three-round socket comparisons on Apple M4 Pro, repeated in reversed variant order:

| Dataset | Order | Before filename median | After filename median | Median change | p90 change |
| --- | --- | ---: | ---: | ---: | ---: |
| Linux | Before, after | 0.06615 ms | 0.04713 ms | -28.8% | -21.7% |
| Linux | After, before | 0.06454 ms | 0.06575 ms | +1.9% | +2.8% |
| Chromium | Before, after | 0.09906 ms | 0.09552 ms | -3.6% | -4.9% |
| Chromium | After, before | 0.09971 ms | 0.09754 ms | -2.2% | -4.2% |

Chromium filename socket gains repeat; Linux socket results are inconclusive.
Classified engine medians improve 1.5-1.9% on Linux and 0-4.9% on Chromium, without a consistent Chromium tail improvement.
No general content, indexing, memory, or whole-disk speedup is established.
Ordered socket responses and engine entry IDs, scores, and totals match across all variants.
Count checks cover 0, 1, 254, exactly 255, larger overflow counts, and missing overflow records.

Local raw measurements and replay workloads are in `results/count-inline-socket.json` and `results/count-inline-engine.json`.
Engine response captures use `results/count-inline-*-engine-*.tsv`.

### Fixed-trip overflow lookup experiment

The `find_key` candidate uses a key-independent iteration count and one conditional select per iteration.
On Apple M4 Pro, Clang already emits conditional selects for the original `std::lower_bound`; the candidate removes one select and some loop bookkeeping rather than eliminating a key-comparison branch.

Six alternating-order microbenchmark pairs on the actual overflow tables, reporting median nanoseconds per call:

| Table | Keys | Before | Candidate | Speed ratio |
| --- | ---: | ---: | ---: | ---: |
| Linux filename overflow | 4 | 3.69 ns | 1.01 ns | 3.64× |
| Chromium directory overflow | 14 | 2.57 ns | 1.59 ns | 1.62× |
| Chromium filename overflow | 27 | 4.46 ns | 1.98 ns | 2.25× |

Three-round filename socket comparisons against the committed version, repeated in reversed order, increase Linux medians by 1.8% in both orders and Chromium medians by 2.1% and 6.4%.
Engine-only median changes range from -7.0% to -0.1% on Linux and reverse from +4.5% to -19.5% on Chromium.
The helper improvement repeats, but these runs do not establish an end-to-end benefit.
The candidate was reverted; production retains `std::lower_bound`.
Do not use the helper speed ratios as search speedup claims.

Exhaustive unique-key checks cover table sizes 0-512, hits, gaps, and unsigned 32-bit extremes.
The candidate returns the last duplicate instead of the first; index writers generate unique keys.
Ordered baseline/candidate results, scores, and totals match. Both existing tests pass.

Repeated fsearch comparisons put candidate filename medians at 0.051-0.062 ms versus 0.136-0.144 ms on Linux and 0.104-0.106 ms versus 0.364-0.390 ms on Chromium.
This cross-engine comparison does not establish a gain from the candidate.
Content coverage still differs for 21 of 67 Linux patterns and 59 of 67 Chromium patterns.
Local raw data: `results/find-key-{lookup,socket,engine}.json` and `results/find-key-vs-fsearch{,-reverse}.json`.

### Split overflow keys and values experiment

An isolated prototype stores sorted 32-bit keys and parallel 64-bit values, using the fixed-trip lookup.
It serializes each array separately for filename counts, large file sizes, and symbol occurrences.
Prototype name and symbol cache magic values are bumped; production formats and code remain unchanged.

A 2×2 microbenchmark separates array layout from lookup algorithm, with all variants non-inlined and identical queries.
For successful lookups on actual overflow tables, splitting arrays while retaining the fixed-trip algorithm gives speed ratios of 0.96× on Linux (4 keys), 0.98× on Chromium directories (14 keys), and 1.01× on Chromium filenames (27 keys).
Mixed hits/misses improve, but ordinary count lookups have recorded keys and hit.
Synthetic million-entry tables show about a 2× layout-only improvement, which does not represent these indexes.

Three-round socket benchmarks against production, repeated in reversed order, change Linux filename medians by +1.1% and -1.1% and Chromium medians by +3.2% and +2.5%.
Engine-only median changes are +0.5% and +1.3% on Linux, while Chromium reverses from -12.2% to +8.4%.
No repeatable end-to-end benefit is established; retain the production layout.
Splitting reduces payload from 16 to 12 bytes per overflow record, but field metadata and alignment increase the Linux cache by 192 bytes and leave the Chromium cache size unchanged.

Ordered results, scores, and totals match. Both existing tests pass on the prototype.
Additional checks cover unique-key sizes 0-512, unsigned extremes, missing-record fallbacks, mismatched key/value lengths, old-cache rejection, mmap round-trip, refresh across the 255-count boundary, symbol overflow, sparse 5 GiB file sizes, and in-memory fallback.

Local raw data: `results/overflow-soa-{layout,socket,engine}.json` and `results/overflow-soa-layout.tsv`.
The prototype patch and experiment sources are archived under `results/overflow-soa-experiment/`.

### Ancestor-exclusion bitmaps

Query-local directory bitmaps avoid repeated ancestor walks for global negation-only queries.
Socket medians improve 29-31% on Linux and 48-49% on Chromium; full-disk engine latency improves about 75% for this class.
Ordinary filename latency and whole-daemon memory changes remain inconclusive.

### Overlapping substring tails

One bounded NEON block replaces the scalar remainder for multi-byte substring searches with at least 16 possible starts.
Annotated baseline assembly attributes about 51% of `find_ci` self samples to the scalar tail, versus 8% to setup.
Captured-input replay improves 17% on Linux and 46% on Chromium.
Reversed-order Chromium socket runs improve overall medians 7-8% and p90 about 19%; Linux socket medians remain unchanged.
Linux engine medians regress 2-4%, and Chromium short-query p90 regresses, so the improvement is workload-dependent.
Needle hoisting and rare-probe selection are not retained.
See the [substring tail measurements](profiling/find_ci.md) for inputs, methods, correctness checks, and limitations.

### Other retained changes

- Eight ranked-search jobs per worker instead of 24: Linux filename socket medians improve about 24-25%. Chromium validation was incomplete.
- Release indexing scratch buffers at last use.
- Parse symbol hits once per file and response snapshot.
- Traverse narrow `in:` subtrees directly.
- Share overlay pages and blocks until mutation.
- Merge sorted content-document streams and pass posting spans without intermediate copies.

These changes do not establish a consistent whole-process memory or content-build throughput improvement.

## Rejected experiments

| Experiment | Reason not retained |
| --- | --- |
| Filter-plan precomputation; single-token NEON masks | Gains disappear or reverse with run order |
| Query-precomputed three-position substring NEON filter | Fewer verifications without consistent end-to-end latency gains |
| Lazy prefix ranking; progressive prefix expansion | Inconsistent latency or worse Chromium tails |
| Fewer filename jobs or threads; caller-only search | No repeatable gain or larger-dataset regressions |
| Typo-bound pruning; worker counter/publishing changes | Inconsistent gains |
| Claim four jobs at a time | Filename medians regress |
| Full ancestor-score propagation | Directory prepass costs exceed saved traversal |
| Selective filename trigram sidecars | Larger-dataset gains do not justify memory and attachment costs |
| Reusable result scratch | Fewer allocations without repeatable socket gains |
| Incremental reads; geometric buffer growth; fewer content readers | Inconsistent medians or worse tails |
| Full content posting intersection | Gains reverse with run order; additional pre-refresh freshness regression |
| Cross-query content buffers | Inconsistent latency and higher peak memory |
| Reader-local path strings | Fewer allocations but slower warmed medians |
| ThinLTO default | Small Chromium benefit; Linux inconclusive, no flag change retained |
| Linux-trained PGO with ThinLTO | Full-disk query classes and p90 regress |

The [three-position substring experiment](results/find-ci-experiment/README.md) records counters, the prototype, and repeated latency comparisons.
The third probe rejects 71% of eligible NEON survivors on Linux and 82% on Chromium, but overall socket medians increase 0.5-1.0%.
Short filenames never enter NEON. Long-filename gains are small or reverse with invocation order; the third probe is not retained.

The [substring caller investigation](results/find-ci-callers-experiment/README.md) separates clean scoring, typo scoring, and other searches.
Typo scoring accounts for 0.127% of substring calls on Linux and 0.046% on Chromium in the same filename workloads.
Verified position-zero matches can avoid another search. Later verified positions require an earlier-prefix search to preserve first-occurrence scoring.
A temporary safe-reuse prototype preserves results and scores; no production change or latency claim follows from these counts.

The retained [content experiment data](results/content-experiments.json) includes measurements, replay sources, and the freshness fixture.
A content K-way merge was investigated but not implemented: sorting was not the measured bottleneck.
Naive worker-batch reuse can race with workers leaving the claim loop; fewer allocations alone do not justify it.

## CPU profile findings

Fresh profiles after the NEON tail change use the same filename and grep requests as the updated fsearch comparison.
Of non-yield filename query samples, Linux spends 40% in `fit_bits` and 17% in candidate enumeration.
Chromium spends 22% in candidate enumeration, 15% in `fit_bits`, and 13% in `find_ci`, plus 4% in its block helper.
Content query samples concentrate at file opens (82-85%), not posting intersections or candidate sorting.
Diagnostic counters show about 9× more fresh-file checks on Linux and 14.5× on Chromium than fsearch.
A temporary four-reader variant reduces diagnostic grep medians without changing results; it is not retained without an acceptance benchmark.
See the [search performance gap investigation](profiling/fsearch_gap.md) for code differences, counters, scope-resolution caching, and coverage limitations.

### Earlier captures

After bit-plane filtering, Linux filename query stacks concentrate in candidate filtering and fuzzy scoring.
Chromium filename stacks concentrate in substring and fuzzy scoring.
Content verification spends about 89-92% of query-stack sampled weight in file reads, mostly file opens; posting intersection is below 0.1%.
These profiles are time-weighted CPU observations, not request-latency decompositions or proof that caching would help.
Scheduler yields during socket pauses reflect the intentional warm-worker policy.

Use the [profiling guide](profiling/README.md) to capture and interpret profiles.
Local captures in `profiles/` retain folded stacks, summaries, and available SVG flamegraphs.
