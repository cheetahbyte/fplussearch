# Chromium: current fplussearch versus fsearch

## Screenshot categories

Apple M4 Pro, macOS, folder-local Chromium indexes. Latency is warmed Unix-socket round-trip p50, including JSON serialization and client parsing.
Two isolated comparisons reverse engine order; each engine runs three rounds per comparison.

| Category | fsearch | fplussearch |
| --- | ---: | ---: |
| Find a file by name, exact queries | 0.379-0.380 ms | 0.105 ms |
| Search inside files | 5.09-5.22 ms | 3.23-3.33 ms |
| Typo still finds the file first | 99.17% | 99.67% |
| Ready after launch, cached index | 12.45 ms | 17.87 ms |
| Memory, Chromium physical footprint after warmed queries | 79.5-90.8 MiB | 138.0-140.7 MiB |

Filename queries are approximately 3.6 times faster with fplussearch. Content queries are approximately 1.6 times faster on this workload, but indexed coverage differs.
fsearch has the lower measured footprint. Cached startup favors its library harness, not necessarily its native daemon.
The screenshot's whole-disk memory and launch figures are not Chromium measurements and are not reused here.

## Important comparability limits

- fplussearch uses its normal `serve --root` executable. fsearch uses the existing folder-local library harness, its production allocator, and its production release profile.
  Its CLI does not offer the same folder-local daemon mode.
- fsearch's harness returns filename paths only; fplussearch serializes additional hit metadata. This favors fsearch's transport cost.
- The cached fsearch harness loads saved indexes without a watcher or background synchronization. fplussearch starts its normal daemon and watcher.
  The startup row therefore measures these two configurations, not equivalent native-daemon or GUI launch behavior.
- Normal thread defaults remain unchanged: eight filename-search threads for fplussearch and twelve for fsearch. fsearch's content read pool uses four threads.
- Content indexes contain 464,869 documents for fplussearch and 381,689 for fsearch. fplussearch also has a separate index with 2,068,021 symbol occurrences.
  Memory and indexing comparisons include these different feature and eligibility choices.
- The OS page cache is not flushed. No existing daemon is stopped or reconfigured. These are local measurements, not confidence intervals or cross-hardware claims.

## Workload and revisions

The existing `standalone-chromium.json` workload is replayed unchanged:
300 unique ASCII exact filenames, 1,200 letter typos, and 67 literal content patterns.
Three rounds produce 900 exact requests, 3,600 typo requests, and 201 content requests per engine per comparison.
Requests return at most 50 results, with one content match per file and no content time budget.
All content requests report completion.

- fplussearch source revision: `5f6cb9b5ca05250ad66ef3f0c52ad02c2718b24b`, plus the uncommitted ancestor-exclusion bitmap change described in [the performance report](performance-experiments.md).
- fsearch revision: `76d612f5b23802eb24926b47e9c4f4ef26884014`.
- Chromium revision: `8b380f69cb2697188431ced311b16cd04e6a3c83`.
- The local Chromium working tree is not pristine: 27 tracked files under `chromeos/ash/components/dbus/hermes` are absent.
  No Chromium files were changed or restored for this comparison. The raw summary records the working-tree status.
- Filename indexes contain 555,626 fplussearch entries and 555,630 fsearch entries.
  The fsearch folder harness adds ancestor entries above the indexed folder.

## Latency by benchmark order

Milliseconds:

| Order | Engine | Exact p50 | Exact p90 | Exact and typo p50 | Exact and typo p90 | Content p50 | Content p90 |
| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |
| fplussearch first | fplussearch | 0.1050 | 0.2585 | 0.0975 | 0.2349 | 3.2341 | 9.4740 |
| fplussearch first | fsearch | 0.3786 | 0.6387 | 0.3600 | 0.6018 | 5.0905 | 20.1628 |
| fsearch first | fsearch | 0.3796 | 0.6457 | 0.3630 | 0.6156 | 5.2198 | 20.5105 |
| fsearch first | fplussearch | 0.1046 | 0.2553 | 0.0980 | 0.2336 | 3.3253 | 9.4383 |

Both engines return the expected filename first for all exact queries.
fplussearch succeeds on 3,588 of 3,600 timed typo requests; fsearch succeeds on 3,570.
Those requests repeat the same 1,200 mutation cases, not 3,600 independent typos.
Accuracy is unchanged in reversed order.

The mixed exact/typo row corresponds to the combined filename workload used by the existing comparison runner.
The screenshot-category table uses exact queries for “find a file by name.”

## Content coverage

After capped timing and memory measurement, each pattern is replayed with a one-million-result limit to compare complete path sets.
Coverage differs for 54 of 67 patterns.

Across pattern/file pairs, there are 422,485 shared matches and 36,979 fplussearch-only matches.
There are no fsearch-only matches in this workload.
These counts repeat exactly in reversed order. A file matching several patterns contributes several pairs; they are not unique-file totals.

Examples:

| Pattern | fsearch matched files | fplussearch matched files |
| --- | ---: | ---: |
| `accessibility` | 9,643 | 10,517 |
| `application` | 14,533 | 17,097 |
| `components` | 65,656 | 71,180 |
| `constructor` | 11,698 | 13,446 |

Consequently, the content latency row is a same-query comparison with different indexed eligibility and recall, not an equal-recall benchmark.
This run does not isolate the reasons for those eligibility differences.

## Cached startup and fresh indexing

Six cached relaunches per engine alternate launch order. Timing begins immediately before spawning the process and ends after a successful known-filename query.
Socket discovery polls every millisecond. Cached indexes and the OS cache are retained.

| Cached filename readiness | fsearch harness | fplussearch daemon |
| --- | ---: | ---: |
| p50 | 12.45 ms | 17.87 ms |
| Minimum | 10.32 ms | 17.46 ms |
| Maximum | 83.37 ms | 20.45 ms |

The first cached fsearch launch is an 83 ms outlier. It is included in the median calculation, not discarded.
Six launches are insufficient to characterize startup tail latency reliably.
Native fsearch daemon startup and GUI launch were not measured.

Fresh indexing is separate:

| Fresh build observation | fsearch harness | fplussearch daemon |
| --- | ---: | ---: |
| Filename index ready | 0.90-0.91 s internally | 0.93-1.40 s to a successful filename response |
| All indexes ready and observable over the socket | 10.80-10.86 s | 24.41-29.22 s |
| Generated index files | 343,704,417 bytes | 627,377,232 bytes |

The fsearch harness binds its socket only after building content; its internal filename-build timer is not an externally searchable readiness time.
The fresh filename row therefore is not a direct latency comparison.
The all-index row includes fplussearch's additional symbol index and larger content corpus.

## Memory

Memory comes from macOS `proc_pid_rusage`.
Physical footprint and resident size are distinct; the screenshot-category row uses physical footprint in MiB, not RSS or decimal MB.
All measurements below precede the uncapped coverage requests.

| Order | Engine | Ready footprint MiB | After capped queries footprint MiB | After capped queries resident MiB |
| --- | --- | ---: | ---: | ---: |
| fplussearch first | fplussearch | 137.3 | 138.0 | 289.4 |
| fplussearch first | fsearch | 73.3 | 79.5 | 253.8 |
| fsearch first | fsearch | 82.3 | 90.8 | 229.8 |
| fsearch first | fplussearch | 139.9 | 140.7 | 329.4 |

The maximum observed footprint after capped requests equals the final footprint in each run.
This sampling does not capture transient in-request peaks or fresh-build peaks.
Neither column establishes whole-disk memory consumption.

## Artifacts and checks

- Compact statistics, workload, executable hashes, startup observations, working-tree status, and coverage hashes: ignored `benchmarks/chromium-vs-fsearch-current.json`.
- Full uncapped path captures: `/tmp/fplussearch-chromium-comparison/full-results.json.gz`.
- Benchmark orchestration and harness preparation: `/tmp/fplussearch-chromium-comparison/run.py` and `/tmp/fplussearch-chromium-prepare.py`.
- Isolated indexes and process logs: `/tmp/fpch`.
- fsearch harness source: `/tmp/fplussearch-chromium-comparison/harness/src/main.rs`.

The Rust harness builds offline with fsearch's release profile: optimization level 3, fat LTO, and one codegen unit.
It copies the current production global allocator. Its only measurement-specific change is optional cached-index loading and additional status fields.
fplussearch builds with the existing `-O3 -mcpu=native` flags.

Ordered filename-path digests match between repeat runs within each engine.
Complete content path sets also match between repeat runs within each engine.
Cross-engine ranking equality is not assumed.
The fsearch repository remains unchanged; this task does not modify either search implementation.
