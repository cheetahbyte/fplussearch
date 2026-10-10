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

Historical Chromium measurements on Apple M4 Pro:

| Metric | fplussearch | fsearch |
| --- | ---: | ---: |
| Exact filename median | 0.105 ms | 0.379-0.380 ms |
| Content median | 3.23-3.33 ms | 5.09-5.22 ms |
| Typo top-result accuracy | 99.67% | 99.17% |
| Cached readiness | 17.87 ms | 12.45 ms |
| Warmed physical footprint | 138.0-140.7 MiB | 79.5-90.8 MiB |

These are historical workload-specific results, not current-binary or whole-disk guarantees.
fplussearch runs its full daemon, watcher, and symbol index; fsearch runs a library harness without equivalent lifecycle work.
Content coverage differs for 54 of 67 patterns, so content timing is not an equal-recall comparison.
Startup and memory figures do not compare equivalent services. Earlier gigabyte-scale fsearch harness footprints were build-buffer retention artifacts.
Raw measurements remain in `results/chromium-vs-fsearch-current.json` when available locally.

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

### Ancestor-exclusion bitmaps

Query-local directory bitmaps avoid repeated ancestor walks for global negation-only queries.
Socket medians improve 29-31% on Linux and 48-49% on Chromium; full-disk engine latency improves about 75% for this class.
Ordinary filename latency and whole-daemon memory changes remain inconclusive.

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
Short filenames never enter NEON. Long-filename gains are small or reverse with invocation order; the production matcher remains unchanged.

The [substring caller investigation](results/find-ci-callers-experiment/README.md) separates clean scoring, typo scoring, and other searches.
Typo scoring accounts for 0.127% of substring calls on Linux and 0.046% on Chromium in the same filename workloads.
Verified position-zero matches can avoid another search. Later verified positions require an earlier-prefix search to preserve first-occurrence scoring.
A temporary safe-reuse prototype preserves results and scores; no production change or latency claim follows from these counts.

The retained [content experiment data](results/content-experiments.json) includes measurements, replay sources, and the freshness fixture.
A content K-way merge was investigated but not implemented: sorting was not the measured bottleneck.
Naive worker-batch reuse can race with workers leaving the claim loop; fewer allocations alone do not justify it.

## CPU profile findings

After bit-plane filtering, Linux filename query stacks concentrate in candidate filtering and fuzzy scoring.
Chromium filename stacks concentrate in substring and fuzzy scoring.
Content verification spends about 89-92% of query-stack sampled weight in file reads, mostly file opens; posting intersection is below 0.1%.
These profiles are time-weighted CPU observations, not request-latency decompositions or proof that caching would help.
Scheduler yields during socket pauses reflect the intentional warm-worker policy.

Use the [profiling guide](profiling/README.md) to capture and interpret profiles.
Local captures in `profiles/` retain folded stacks, summaries, and available SVG flamegraphs.
