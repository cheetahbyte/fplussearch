# Further search performance experiments

## Result

Retain one algorithmic change: a query-local ancestor-exclusion bitmap for global queries with negative tokens and no positive tokens.
Examples include `!test`, `type:file !test`, and `ext:cpp !third_party`.
The bitmap replaces repeated parent-chain traversal with one directory-preorder lookup.

Negation-only socket medians improve 29-31% on Linux and 48-49% on Chromium in both variant orders.
Whole-disk engine medians for this class improve approximately 75%.
These are targeted gains, not a general improvement to ordinary 50-150 µs filename searches.
Ordinary filename medians remain inconsistent across comparisons.

Full ancestor-score propagation, three filename-trigram prototypes, and reusable result scratch were rejected.
Compiler experiments remain optional experiments; default compiler flags are unchanged.
Content sorting was investigated but not changed because fresh file access dominates the current profile.

## Method

Baseline source: `5f6cb9b5ca05250ad66ef3f0c52ad02c2718b24b`.
The changed binary uses that source plus the retained `src/search.cpp` diff.
Measurements use Apple M4 Pro, Apple Clang 21.0.0, eight search threads, and `-O3 -mcpu=native`.

Datasets:

| Dataset | Entries | Directories | Source revision |
| --- | ---: | ---: | --- |
| Linux source | 102,369 | 6,298 | `6c377d19d4a5116d9bec5203aa3c6c11523e7898` |
| Chromium source | 555,626 | 46,572 | `8b380f69cb2697188431ced311b16cd04e6a3c83` |
| Existing full-disk snapshot | 6,316,253 | 899,032 | Immutable copy of the existing filename index |

The saved standalone workloads supply 300 exact queries and 1,200 typos per dataset.
Seed 71 supplies sampled directory/filename pairs for multi-token and mixed-negation queries.
Fixed additions cover short queries, repeated tokens, eight positive tokens, and more than 16 negative tokens.
The final workload has 1,812 queries, including 40 negation-only cases.
Each timed invocation runs three rounds: 5,436 filename requests, including 120 negation-only requests.
Socket runs also replay 67 literal content patterns three times, with 50 results and no time budget.
Earlier experiments use the same workload without the final 40-query class.

Variants run sequentially, with reversed order in a separate comparison.
No profiling, compilation, or second benchmark variant runs during timing.
Engine comparisons load identical immutable indexes. Socket comparisons start isolated daemons and complete all index passes before timing.
Existing daemons remain untouched. The OS page cache is not flushed.

Engine timing excludes parsing, transport, and serialization. Socket timing includes them.
CPU seconds include all harness threads, worker yielding, and bookkeeping during the query loop.
Peak RSS includes index loading and query execution; it is not an allocation counter or physical footprint.
Small query classes have fewer samples than exact and typo classes.
These local measurements have no confidence intervals and do not establish performance on other hardware.

Raw statistics, workload text, hashes, readiness times, and memory observations are in the ignored local file `performance-results.json`.
Complete response captures and rejected prototype sources remain in `/tmp/fplussearch-performance`.
Compiler builds and training scripts remain in `/tmp/fplussearch-compiler-rj7Jxc`.
Temporary artifacts are not a durable substitute for the reproduction tools described here.

## Current profiles

Read the earlier reports before experimenting, including [rejected optimizations](optimizations.md),
[median latency experiments](median-latency.md), [bit-plane results](bit-sliced-mask-results.md),
and [bounded substring matching](substring-pair-results.md).

New Time Profiler captures use the current baseline, not the earlier pre-substring implementation.
Startup samples are excluded. Inclusive percentages overlap.

| Capture | Finding |
| --- | --- |
| Linux daemon, ordinary filename workload | Mask filtering contains 38.2% of filename-query sample weight; name matching contains 20.3%. |
| Chromium engine, multi-token and mixed-negation workload | Ancestor scoring contains 5.9% of all sampled CPU; name matching contains 62.2%. |
| Whole-disk engine, negation-only workload | Ancestor scoring contains 69.3% of query sample weight. Parent decoding contains 16.7%, nested within scoring. |
| Linux daemon, content workload | Fresh file reads contain 92.0% of query sample weight; file opens contain 81.2%. |

These captures justify specializing the broad negation path, not adding a full directory-score prepass to every multi-token query.
They do not establish that pool allocation or content sorting dominates latency.
Syscall samples do not separate kernel CPU, filesystem contention, and off-CPU waiting.

## Retained ancestor-exclusion bitmap

Directory IDs already have depth-first preorder positions and subtree ends.
A negative directory match excludes its entire preorder interval from ancestor acceptance.
Workers atomically union these intervals into a packed bitmap.
After the directory batch completes, entry scoring reads the parent directory's bit without walking its ancestors.

Directory candidate filtering uses the existing mask planes before unchanged negative-token verification.
More than 16 negative tokens fall back to complete directory-name enumeration, avoiding truncated-mask false negatives.
Positive-token scoring, narrow scoped traversal, overlay scoring, ranking keys, and result counting retain their existing paths.
The optimization also covers `each_file`.

### Engine latency

Negation-only latency, in microseconds:

| Dataset and order | Baseline p50 | Bitmap p50 | Baseline p90 | Bitmap p90 |
| --- | ---: | ---: | ---: | ---: |
| Linux, baseline first | 357.3 | 244.8 | 427.0 | 319.8 |
| Linux, bitmap first | 357.5 | 222.1 | 431.6 | 265.7 |
| Chromium, baseline first | 2,225.3 | 1,075.0 | 2,715.1 | 1,631.4 |
| Chromium, bitmap first | 2,157.0 | 1,085.8 | 2,506.1 | 1,632.6 |
| Full disk, baseline first | 29,926.0 | 7,456.3 | 31,314.5 | 8,831.8 |
| Full disk, bitmap first | 30,196.3 | 7,545.9 | 31,923.0 | 8,866.8 |

Whole-process CPU during the full-disk mixed workload falls from 52.94 to 34.62 seconds,
and from 54.17 to 35.34 seconds in reversed order, approximately 35% in both comparisons.
Chromium CPU falls 9-12%. Linux CPU observations are inconsistent in magnitude.
These percentages depend on this workload's frequency of negation-only requests.

### Socket latency

Negation-only latency, in microseconds:

| Dataset and order | Baseline p50 | Bitmap p50 | Baseline p90 | Bitmap p90 |
| --- | ---: | ---: | ---: | ---: |
| Linux, baseline first | 460.7 | 319.7 | 593.5 | 384.3 |
| Linux, bitmap first | 456.3 | 325.3 | 572.3 | 417.3 |
| Chromium, baseline first | 2,326.5 | 1,215.4 | 2,711.7 | 1,760.8 |
| Chromium, bitmap first | 2,438.4 | 1,242.5 | 2,796.8 | 1,790.4 |

Overall socket medians change from 48.21 to 46.79 µs and 47.88 to 47.25 µs on Linux.
Chromium changes from 107.25 to 107.63 µs and 107.58 to 106.06 µs.
Do not interpret those small aggregate differences as a general filename improvement.

### Memory and indexing

Directory-hit payload changes from ten bytes per directory to one bit per directory for eligible queries:

| Dataset | Previous temporary payload | Bitmap payload |
| --- | ---: | ---: |
| Linux | 62,980 bytes | 792 bytes |
| Chromium | 465,720 bytes | 5,824 bytes |
| Full disk | 8,990,320 bytes | 112,384 bytes |

No persistent index data, sidecar, disk-format field, or indexing step is added.
Fresh daemon readiness remains approximately 7.5-8.1 seconds on Linux and 22.5-23.5 seconds on Chromium in final comparisons.
Readiness includes filename, symbol, and content indexing; these observations do not isolate indexing throughput.
Total generated index-file sizes match between variants.
Daemon RSS differences change sign across runs. No reliable whole-daemon memory reduction is claimed.

### Content timing qualification

Content code is unchanged, but final Chromium content p50 rises from 3.264 to 3.650 ms in one reversed comparison.
Its p90 rises from 9.349 to 12.818 ms. That observation cannot be omitted from regression assessment.

An additional content-only baseline/bitmap/bitmap/baseline sequence does not reproduce it.
Medians are 3.443/3.271 ms and 3.496/3.393 ms; p90 is 9.932/9.393 ms and 11.430/9.269 ms.
Complete matching results remain identical. Content variability remains a limitation; no content speedup is claimed.

## Rejected full ancestor-score propagation

The first prototype retains the existing directory-hit array, then propagates positive bits, negative flags,
and four best scores in preorder. Entry scoring becomes one parent lookup.

The added directory pass costs more than the eliminated traversal on the tested multi-token workloads.
All ordered results and totals match.

| Dataset | Baseline/prototype p50, baseline first | Baseline/prototype p50, prototype first |
| --- | ---: | ---: |
| Linux | 51.5/57.8 µs | 45.6/51.5 µs |
| Chromium | 701.6/749.9 µs | 714.7/757.2 µs |
| Full disk | 2,066.7/3,817.2 µs | 2,073.5/3,899.2 µs |

Reject the full prepass. It reads and updates every directory, including directories irrelevant to selective matches.
The retained bitmap avoids score propagation and targets the class where traversal actually dominates.

## Rejected selective filename trigrams

Temporary index-owned sidecars store postings over distinct names, including decoded hex names.
Names are case-folded with the existing ASCII byte table.
Common grams discard their postings. Original masks, matching, scoring, deletion handling, and overlay scanning remain verifiers.
Short, multi-token, and negated queries retain the normal scan.

Three prototypes were compared:

1. Cap postings at 128 names. For typo-enabled queries of at least nine bytes, union postings from three disjoint query pieces.
2. Raise the same cap to 1,024 names.
3. Restore the 128-name cap and union five selective query windows for typo-enabled queries.
   One edit can disrupt at most four original trigram windows, including adjacent transposition.
   Queries without enough indexed windows fall back to scanning.

The 128-name variants improve Linux engine medians but do not improve Chromium or whole-disk medians reproducibly.
The five-window variant slightly regresses both larger datasets in both orders.

The 1,024-name variant improves mixed engine medians approximately 23-26% on Linux,
8% on Chromium, and only 1-2% on the full-disk snapshot.
Peak RSS rises approximately 10 MiB, 16 MiB, and 75-79 MiB respectively.
Warm index attachment adds approximately 23 ms, 220 ms, and 1.25 seconds respectively.
These costs are not justified by the small whole-disk improvement. Reject all three variants.
No prototype enters the production index or cache format.

One early disk parity check failed when an existing seven-day mtime bonus expired between runs.
The affected entry's stored timestamp confirms that boundary. Repeated comparisons after the boundary match exactly.
Other full ordered filename responses and totals match across the measured prototype workloads.
The trigram prototypes did not receive exhaustive adversarial typo or lifecycle validation, so they are not production-ready implementations.

## Rejected reusable result scratch

The prototype reuses per-job `TopK` buffers and the combined key vector in guarded thread-local scratch.
Limits above 256 retain query-local buffers. Result objects remain independently owned.

A separate allocation probe counts global and aligned `operator new` calls during search, not during parsing or harness bookkeeping.
It excludes large `MapAlloc` allocations served directly by `mmap` and allocations inside external libraries.
Diagnostic runs are not latency evidence.

On Linux, exact-query median allocation calls fall from 16 to 13.
For the broad negation-only class, they fall from 561 to 41.
However, socket filename medians change from 47.959 to 48.083 µs and 47.542 to 45.542 µs on Linux.
Chromium changes from 104.229 to 105.042 µs and 103.625 to 103.896 µs.
This does not establish a repeatable end-to-end latency gain. Reject the scratch prototype and its retained per-thread memory.
The allocation diagnostics include the added negation-only class; these scratch socket runs predate that addition.
Its targeted latency after the retained bitmap remains unmeasured.

`Pool::run` allocates one shared batch per invocation.
Completion of all jobs does not establish that every worker has stopped accessing that batch.
Naive batch reuse can race with workers still leaving the claim loop.
A safe replacement needs worker-generation acknowledgement or equivalent ownership.
The profile does not justify that synchronization change for one small allocation.
No pool policy, job target, or batch-lifetime change is retained.

## Content selection investigation

Current candidate streams are not sorted by search rank, modification time, and path.
Physical document IDs and the path-sorted permutation therefore cannot directly supply a rank-ordered K-way merge.
Synchronization already merges path streams; that is a different operation.

Sorting each segment first and merging changes work from `O(C log C)` to
`O(sum(c_i log c_i) + C log S)`, where `C` is candidate count and `S` is segment count.
The existing verifier also requires concurrent random access to candidate positions.
A shared mutable merge iterator would violate that interface; flattening a merge retains the extra candidate array.

The current content profile concentrates at fresh file opens, not candidate sorting.
Earlier lazy-prefix ranking trials already failed acceptance.
Defer a K-way merge until a workload demonstrates material sorting cost.
No content cache, freshness change, selection prototype, or content speedup is claimed.

## Compiler experiments

All variants use unchanged baseline sources. Training uses only the Linux filename corpus for two seconds with four replay threads.
Chromium and the disk snapshot are held out. Timed runs use eight threads and the mixed workload.

Engine mixed-workload p50, in microseconds:

| Dataset and order | Default | ThinLTO | PGO | PGO with ThinLTO |
| --- | ---: | ---: | ---: | ---: |
| Linux, default first | 17.62 | 16.54 | 14.92 | 15.29 |
| Linux, default last | 13.48 | 13.79 | 12.83 | 13.38 |
| Chromium, default first | 70.25 | 65.08 | 66.38 | 60.31 |
| Chromium, default last | 66.67 | 63.67 | 69.92 | 56.90 |
| Full disk, default first | 250.79 | 244.48 | 226.67 | 218.23 |
| Full disk, default last | 260.85 | 254.23 | 229.98 | 228.04 |

ThinLTO socket medians are 47.959/48.041 µs and 47.542/46.541 µs on Linux,
and 104.229/102.791 µs and 103.625/99.688 µs on Chromium, default/ThinLTO respectively.
Its Chromium benefit is small but repeats. Linux is inconclusive; no default flag change is retained.

PGO with ThinLTO improves Chromium engine medians and ordinary disk queries,
but disk multi-token and mixed-negation medians regress approximately 5-9%.
Disk mixed-workload p90 rises approximately 9-12% in both orders.
Reject this Linux-only training profile as a universal default, not PGO in general.

Build flags were verified against installed Clang help:
`-flto=thin`, `-fprofile-instr-generate`, and `-fprofile-instr-use`.
`xcrun llvm-profdata merge` combines training profiles.
Combined daemon/replay/harness builds take approximately 14 seconds for default and 15 seconds for ThinLTO.
Instrumentation takes 16 seconds; profile-use variants take 14-15 seconds, excluding two-second training.
These are coarse whole-second observations.
Daemon binary sizes are 1,315,304/1,282,456/1,301,832/1,243,800 bytes in table order.

PGO emits one function-profile mismatch out of 46 functions and missing-profile warnings for the TUI and timing harness.
The training does not cover content, serialization, indexing, or interactive use.
Representative training and content timing are required before adopting a PGO build.
Complete engine filename results match across all compiler variants; ThinLTO socket matching results also match.
PGO socket latency was not measured.

## Verification

- Final engine and socket comparisons match complete ordered filename responses, scores, and total counts in both orders.
- Socket content paths, lines, candidate counts, and completion flags match.
  Diagnostic `read` counts are excluded because concurrent limited verification can vary them.
- A temporary fixture checks 192 queries with one and eight threads, limits 0, 1, 50, 500, and 10,000,
  deletion overlays, new overlay paths, narrow scopes, ancestors above scope, root exclusions, duplicate names,
  hex names, Unicode bytes, prefix/suffix negatives, and more than 16 negatives.
  All 3,840 ranked responses and 384 sorted `each_file` responses match the baseline.
- The same fixture passes with the changed search implementation and verifier compiled under AddressSanitizer and UndefinedBehaviorSanitizer.
  Other library objects are not sanitizer-instrumented.
- `make test-search`, `make lib`, daemon-disconnect checks, and Swift debug/release builds with warnings as errors pass.
- The profiling tools build and the new measurement executable passes a workload smoke run.
  `dsymutil` retains the existing replay warning about its temporary compilation object.
- The workload generator reproduces both measured TSV files byte-for-byte.

No permanent tests were added. Live filesystem refreshes and concurrent index replacement were not separately stress-tested.
The bitmap is rebuilt per query from the selected immutable index; no cross-query ancestor cache is introduced.

## Reproduce engine comparisons

Replace `WORKLOAD.json` with a saved standalone benchmark containing its dataset root.
Replace `INDEX.bin` with an existing compatible immutable filename index.
The measurement executable only loads it; it never rebuilds or writes it.
Use separate, empty build output directories. Generate the workload once and reuse that file for all variants:

```sh
python3 -B benchmarks/profiling/workload.py WORKLOAD.json /tmp/fplussearch-workload.tsv
make -f benchmarks/profiling/Makefile OUT=/tmp/fplussearch-before -j8
```

After applying a candidate, build it in a separate output directory:

```sh
make -f benchmarks/profiling/Makefile OUT=/tmp/fplussearch-after -j8
/tmp/fplussearch-before/measure INDEX.bin /tmp/fplussearch-workload.tsv 3 50 8 \
  /tmp/before.responses > /tmp/before.json
/tmp/fplussearch-after/measure INDEX.bin /tmp/fplussearch-workload.tsv 3 50 8 \
  /tmp/after.responses > /tmp/after.json
cmp /tmp/before.responses /tmp/after.responses
/tmp/fplussearch-after/measure INDEX.bin /tmp/fplussearch-workload.tsv 3 50 8 \
  /tmp/after-repeat.responses > /tmp/after-repeat.json
/tmp/fplussearch-before/measure INDEX.bin /tmp/fplussearch-workload.tsv 3 50 8 \
  /tmp/before-repeat.responses > /tmp/before-repeat.json
cmp /tmp/before-repeat.responses /tmp/after-repeat.responses
```

The TSV format is `CLASS`, a tab, then a query. `all` is a reserved class name.
Unlike the profiling replay driver, `measure` does not prepend an `in:` scope.
Use explicit scopes in the TSV when needed. The same unscoped source-tree workload can exercise a full-disk snapshot.
Optional response output contains totals, ordered entry IDs, and scores for the first round.
Aging bonuses can cross time boundaries even with an unchanged index; investigate such mismatches rather than ignoring them.

For socket timing, use the [standalone benchmark procedure](README.md#repeat-the-same-workload)
and keep invocations at three rounds or fewer. The standalone runner's standard workload does not include the added query classes.
The classified socket comparisons in this report use the temporary runner recorded with the raw artifacts.
