# Indexing and search optimizations

These changes use commit `a0ab635a3d67d9df2c6cf15dc10b92bee05d644c` as the baseline.
They preserve the disk format and JSON response fields.

## Changes

1. Release filename-index scratch buffers at their last use. Empty initializer-list assignment retained their capacity; empty-vector swaps release it.
2. Group symbol hits by file and parse each file once per response. Single-hit lookups retain the existing path.
3. Traverse narrow `in:` subtrees directly. The candidate limit is the smaller of 16,384 entries and one-sixteenth of the index.
4. Share deletion-bitmap pages and sorted overlay-map blocks between snapshots. Mutations copy affected pages or blocks.
5. Merge sorted content-segment document streams during synchronization. This removes the combined document list and its sort.
6. Pass posting spans directly to the segment writer and reuse insertion offsets. This removes posting copies and a separate offset array.

Broad or empty scopes retain global scanning. Scoped ancestor matching includes directories above the scope.
Symbol batching uses a response-local file snapshot, without a persistent cache.
The response's `ms` field still excludes symbol location lookup and JSON serialization.

Overlay snapshots still copy their page and block pointer directories.
Publishing still creates a flat pointer list for queries, and bitmap reads add an indirection.
Content extraction still retains per-thread trigram buffers and grouped shard buffers; this change does not eliminate their overlap.

## Targeted measurements

These are local measurements, not general latency guarantees.

| Workload | Baseline | Updated | Qualification |
|---|---:|---:|---|
| Serialize 100 symbol hits from one file with 1,000 definitions | 12.43 ms | 0.176 ms | Median of 20 alternating repetitions; identical JSON |
| Copy 50,000 overlay entries and change one entry, repeated 100 times | 86.15 ms | 0.47 ms | Container microbenchmark; excludes publication and filesystem events |
| Scoped search typing workload, 107-entry scope in a 15,158-entry index | 0.100 ms | 0.077 ms | One integrated run; hot latency was 0.062 versus 0.064 ms |

A source-derived allocation check retained 80 MiB before the buffer-release fix and zero bytes afterward.
This checks capacity release, not whole-process memory.

A content-only build trial used 512 files of 64 KiB each.
Peak resident memory was approximately 167.4 MiB in both versions; elapsed time was 0.36 versus 0.41 seconds.
Extraction dominated that trial's peak. No content-build throughput or peak-memory improvement is claimed from that measurement.

## Dataset comparisons

Linux and Chromium ran in isolated daemon processes, with three workload rounds per variant and both variant orders.
Complete filename and content result digests matched in both orders, including scores and matching lines.

Median socket round-trip times, in milliseconds:

| Dataset | Order | Filename baseline | Filename updated | Content baseline | Content updated |
|---|---|---:|---:|---:|---:|
| Linux | Baseline first | 0.067 | 0.065 | 1.191 | 1.180 |
| Linux | Updated first | 0.059 | 0.078 | 1.202 | 1.146 |
| Chromium | Baseline first | 0.135 | 0.126 | 4.396 | 3.983 |
| Chromium | Updated first | 0.146 | 0.145 | 4.373 | 4.188 |

Four additional Linux runs used ten rounds each, in baseline/updated/updated/baseline order.
Filename medians were 0.059, 0.061, 0.056, and 0.057 ms.
Content medians were 1.001, 1.168, 1.058, and 1.021 ms.
These results do not establish a general query-speed improvement.

After the workload, Linux's sampled physical footprint decreased by 2.4-9.1 MiB.
Chromium's changed from a 1.4 MiB decrease to a 3.7 MiB increase between runs.
These samples are not allocation peaks and do not establish a consistent whole-process memory reduction.

Use the [standalone benchmark instructions](README.md#repeat-the-same-workload) to preserve a baseline and replay identical queries.
Reverse the variant order for a second comparison. Avoid concurrent benchmark processes.
The scoped-search and repeated-symbol workloads need separate targeted measurements; the standard workload does not cover them.

## Verification and limitations

The integrated build passed `make test-search`, `python3 -B tests/test_daemon_disconnect.py`, and `swift build`.
Temporary checks covered symbol batching, copy-on-write snapshots, scoped-search parity, content synchronization, reopening, and segment merging.

A live filesystem comparison checked 144 filename and symbol responses at each of four stages:
initial indexing, additions/edits/deletions, subtree rename, and subtree deletion.
Responses matched the baseline throughout.

That comparison exposed an existing correctness issue in both versions: renaming an indexed directory with overlay entries can leave stale entries.
The fixture returned 700 hits for 620 files after rename, then 80 stale hits after deletion.
This optimization change does not fix that separate issue.
