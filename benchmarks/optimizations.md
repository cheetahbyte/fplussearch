# Optimization experiments

No attempted optimization consistently beats the pre-experiment baseline on both datasets. All engine changes from these experiments are reverted.
The existing, uncommitted posting-intersection change in `src/content.cpp` remains untouched and is included in the baseline.

## Trials

| Trial | Finding | Decision |
|---|---|---|
| Rank a candidate prefix, then sort the remaining candidates lazily | Inconsistent Chromium latency | Reject |
| Expand the ranked prefix progressively | Better medians in one run, worse Chromium average and p90 | Reject |
| Reduce filename jobs from 24 to 4 per worker | No useful improvement; worse Chromium tail latency | Reject |
| Read literal content incrementally, with 64 KiB chunks | Lower average latency, not consistently lower medians | Reject |
| Read an initial 8 KiB, then grow the buffer geometrically | Worse Chromium average latency | Reject |
| Run selective filename searches on the caller thread | Large Chromium regression | Reject |
| Reduce content readers from 12 to 4 or 8 | Some average and tail improvements, but inconsistent median latency | Reject |
| Add an explicit NEON path for single-token mask filtering | Initial Chromium improvement disappears with reversed run order | Reject |

## Final trial versus baseline and fsearch

The final NEON trial uses five passes per variant and dataset: 7,500 filename queries and 335 content queries.
Only one benchmark engine runs at a time, avoiding contention between hot worker pools.
A second independent run reverses the variant order.

Median filename latency:

| Dataset and run | Baseline | NEON trial | fsearch |
|---|---:|---:|---:|
| Linux, baseline first | 0.062 ms | 0.063 ms | 0.142 ms |
| Linux, fsearch first | 0.059 ms | 0.060 ms | 0.133 ms |
| Chromium, baseline first | 0.142 ms | 0.123 ms | 0.379 ms |
| Chromium, fsearch first | 0.132 ms | 0.144 ms | 0.368 ms |

The apparent Chromium improvement reverses into a regression. The trial fails the acceptance criterion despite remaining faster than fsearch.
No content optimization is active in these final runs; content latency differences aren't evidence of an optimization.

[Forward-order results](optimization-results.json) and [reverse-order results](optimization-repeat.json) preserve medians, p90, means, per-pattern statistics, and memory samples.
The trial code is not retained. These files document a rejected experiment, not the current binary's performance.

## Memory

The Rust harness now uses fsearch's production global allocator and its post-build allocator cleanup.
Previously observed gigabyte-scale harness footprints were build-buffer retention artifacts, not valid production-memory comparisons.
The harness also drops the temporary index and content-document list after building.

Physical footprint after the reverse-order workload, in MiB:

| Dataset | Baseline | Rejected NEON trial | fsearch harness |
|---|---:|---:|---:|
| Linux | 114.6 | 108.8 | 40.8 |
| Chromium | 161.9 | 159.9 | 87.1 |

The JSON files also record resident memory, readiness samples, and maximum observations after requests.
These observations aren't allocation peaks: short-lived allocations can disappear before sampling.
macOS physical footprint and resident memory measure different things; clean mapped pages and memory compression affect them differently.

fplussearch runs its full daemon, including symbol indexing. fsearch runs a library harness without its complete daemon lifecycle.
They retain their different content-eligibility policies. Their memory numbers aren't equivalent-service measurements.
The NEON trial adds no persistent cache or heap allocation, but measured footprints vary between independent runs.
No reliable memory reduction is claimed.

## Reproduce a candidate comparison

Before changing the engine, preserve the baseline executable:

```sh
make -j8
cp build/fplussearch /tmp/fplussearch-baseline
```

After implementing a candidate, run:

```sh
python3 benchmarks/compare.py ~/fplussearch-bench --baseline /tmp/fplussearch-baseline --rounds 5 --out /tmp/forward.json
python3 benchmarks/compare.py ~/fplussearch-bench --baseline /tmp/fplussearch-baseline --rounds 5 --reverse --out /tmp/reverse.json
```

The benchmark defaults to isolated processes. `--no-isolate` enables the earlier concurrent-process comparison.
Baseline comparisons check complete ordered filename responses and unlimited content results, including matching lines.
Variant timing remains subject to source-file page-cache warming and unrelated background activity.

Verification: full result digests match baseline in both final runs. A temporary randomized check validates 200,000 mask comparisons.
The normal engine is rebuilt after reverting the trial; the existing daemon-disconnect regression check passes.
