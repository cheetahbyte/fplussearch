# Search performance gap investigation

Updated fsearch avoids substantially more fresh-file verification work during grep.
Its filename search also has stronger candidate filters and cached scope resolution.
Those filename differences are verified in code, but their individual contributions to latency remain unmeasured.

No production implementation changes follow from this investigation.
The four-reader variant exists only in a temporary diagnostic build.

## Versions and method

Source paths and line numbers below refer to these revisions, before the source-directory reorganization.

- fplussearch: `95a48cf`, including the overlapping NEON substring tail.
- fsearch: `d753915`, with a clean checkout.
- Workloads: the same Linux and Chromium queries from `fsearch-updated-forward.json`.
- Each dataset: 1,500 filename queries, including full filenames and one-edit typos, plus 67 literal grep patterns.
- Request limits: 50 results, one content match per file, and no content time budget.

Exact-filename controls use plain queries, not quoted exact-mode tokens; both engines parse them as fuzzy queries.

Four fresh Time Profiler captures attach to an isolated fplussearch daemon: filename and content searches on each dataset.
The profiling build uses normal `-O3 -g -mcpu=native` flags without counters.
Analysis resolves XML references and uses running samples between seconds 1 and 11.
Filename and grep captures are separate; their percentages cannot be combined.

The following query percentages exclude scheduler yielding and samples outside `Engine::search` or `ContentIndex::grep`.
They measure sampled execution stacks, not request wall time.
Inclusive percentages overlap; the tables use leaf/self percentages instead.

## fplussearch hot spots

### Filename search

| Leaf region | Linux query samples | Chromium query samples | Source |
| --- | ---: | ---: | --- |
| Character-presence filtering, `fit_bits` | 40.4% | 15.3% | `src/search.cpp:321` |
| Candidate enumeration, `Scan::candidates` | 17.0% | 21.8% | `src/search.cpp:581` |
| Substring matching, `find_ci` | 7.1% | 13.1% | `src/search.cpp:266` |
| NEON block helper within `find_ci` | Outside the top 25 leaves | 3.8% | `src/search.cpp:278` |
| Name/entry counts, `Section::count` | 2.8% | 4.9% | `src/index.hpp:105` |
| Token score bounds, `token_bound` | Outside the top 25 leaves | 4.8% | `src/search.cpp:397` |

Candidate filtering and enumeration together exceed the direct substring-matching share on both datasets.
Enumeration also walks counts of skipped names to locate the first matching entry (`src/search.cpp:602`).
The count and substring-helper rows are separate self samples, not additions to inclusive parent percentages.

Outside useful query work, worker hot-loop yielding accounts for 66.2% of Linux filename process samples and 35.3% on Chromium.
That path is `Pool::loop` at `src/pool.hpp:79`.
Waiting for a batch to complete contributes another 0.7-0.9% of process samples.
The hot-loop policy intentionally keeps workers ready for typing; these shares do not establish equivalent request-latency savings.

### Grep

| Leaf region | Linux query samples | Chromium query samples |
| --- | ---: | ---: |
| `__open` | 82.4% | 84.7% |
| `read` | 6.4% | 5.4% |
| Content substring matcher | 2.5% | 2.7% |
| `close` | 1.4% | 1.0% |
| `fstat` | 1.2% | 0.9% |

The relevant call chain is `ContentIndex::grep` (`src/content.cpp:1517`), `verify` (`:1478`), `match_file` (`:591`), and `read_file` (`:168`).
The file-opening boundary dominates sampled stacks, even with a warm workload.
These captures do not separate syscall CPU work from off-CPU I/O waits or endpoint-security overhead.

Posting intersection and candidate sorting do not appear among the dominant sampled query hot spots.
Optimizing either without reducing file verification is unlikely to address the main measured grep cost.
The content matcher is separate from the filename matcher whose tail was optimized earlier.

## Why fsearch checks fewer files

fsearch stores content postings over deduplicated file contents, then maps content IDs back to document paths.
Before opening a path, it checks Bloom filters for the pattern's five- and seven-byte grams.
The check is `may_contain` at `../fsearch/src/content.rs:195`, called by `Content::search` at `:1844`.
A failed check returns no path to `verify_from`, so no file verification is attempted (`:2426`).

fplussearch uses trigram postings with follow-character buckets, then verifies surviving paths directly.
It has no equivalent five/seven-byte Bloom check in the active content query path.
It also skips expensive posting intersections once only four candidates remain (`src/content.cpp:1428`).
The latter deliberately trades false candidates for less decoding work.

A diagnostic harness exposes the existing `GrepResult.candidates` and `GrepResult.read` fields for both engines.
It warms all 67 patterns, then runs three rounds in forward and reversed engine order.
`read` counts file-verification attempts, not individual `read()` syscalls or Bloom tests.

| Mean file-verification attempts per query | Linux | Chromium |
| --- | ---: | ---: |
| fplussearch, 12 readers | 325.0 | 649.0 |
| fplussearch, four readers | 320.8 | 642.9 |
| fsearch, four readers | 35.0 | 44.7 |
| fplussearch/fsearch ratio, default readers | 9.3× | 14.5× |

Raw candidate totals are relatively close; the large difference appears between candidate selection and fresh verification.
For Linux `xt_helper_info`, both engines return exactly the same two ordered file/line results.
fplussearch verifies 893 files per request; fsearch verifies three.
For `intr_status`, the same 50 ordered results require about 936 checks in fplussearch and 74 in fsearch.
These examples show that different coverage is not the entire explanation for the extra verification work.

Bloom filtering, deduplication, candidate ranking, and eligibility were not ablated independently.
Their exact individual shares of the end-to-end speedup remain unknown.

### Reader-count diagnostic

fplussearch defaults to 12 readers (`src/content.hpp:57`, instantiated at `src/overlay.cpp:93`).
fsearch caps indexed verification at four (`../fsearch/src/content.rs:2368`).
Its source comments attribute that limit to file-open contention; that attribution is not independently established for this machine.

Changing only the temporary fplussearch construction to four readers gives:

| Diagnostic change | Linux | Chromium |
| --- | ---: | ---: |
| Median latency, first order | -32.6% | -6.7% |
| Median latency, reversed order | -18.8% | -1.8% |

All ordered file/line responses match between the 12- and four-reader variants across every round and invocation.
The four-reader variant lowers diagnostic median latency, but barely changes how many files get checked.
It cannot explain the remaining 9-14× verification-count difference.

These measurements use complete pattern warmup and additional counter fields in the fsearch response.
They are diagnostic evidence, not replacements for the original comparison or a production acceptance benchmark.
Earlier reader-count experiments were inconclusive; retain the production default until a dedicated acceptance run supports changing it.

## Filename implementation differences

1. **Stronger bitmap prefilters.** `TokenBits::words` tests character classes in each name half, repeated classes, and class order before loading names (`../fsearch/src/query.rs:128`).
   It also processes rare classes first.
   fplussearch's `fit_bits` tests character presence and typo-start hashes, then sends survivors to scoring.
   This exchanges a larger persistent bitmap index for potentially fewer candidate names and scoring calls.
   Survivor counts under the shared workload have not yet been compared.
2. **Cached `in:` resolution.** fsearch caches the last canonicalized scope for one second (`../fsearch/src/query.rs:482`).
   fplussearch calls `realpath` for every scoped query (`src/search.cpp:988`).
   Every benchmark request uses the same dataset scope, making the cache directly relevant.
   Upstream documents a few microseconds per resolution; this investigation does not independently time that saving.
   A one-second cache also delays observing symlink changes.
3. **Whole-name NEON fuzzy scoring.** fsearch loads up to 64 name bytes and creates position masks for query bytes (`../fsearch/src/query.rs:673`).
   fplussearch first locates a contiguous substring, then scores its bytes and tries separate typo helpers.
   fsearch's prefix fast path bypasses its masked scorer for many target names, so masked-scoring savings need path counters.
4. **Different fuzzy semantics.** fsearch permits subsequence matches (`../fsearch/src/query.rs:656`); fplussearch requires contiguous matches.
   fplussearch additionally handles whole-name one-edit typos (`src/search.cpp:253`).
   Copying fsearch's scorer would change matching and ranking, not just execution cost.

Conditional ranked scans, short-token lists, and cached name tables are additional fsearch features.
Short-token lists do not explain this full-filename workload, whose selected names are at least six bytes.
The share of requests using the other fast paths is unmeasured.

## Coverage and benchmark limits

fsearch excludes more extensions and directories, including `build`, `dist`, `vendor`, and `Library` components.
fplussearch retains a broader text-file eligibility policy.
Content file counts differ on 21 Linux patterns and 59 Chromium patterns, always with more files returned by fplussearch.
Limited-result tie-breaking and displayed line lengths also differ.
Therefore, the original grep latency comparison is not an equal-recall comparison.

The fsearch benchmark uses a library harness; fplussearch runs its full daemon, watcher, and symbol index.
Socket parsing, query parsing, and serialization contribute to measured latency.
No percentage decomposition of the filename latency gap is established by these code comparisons.

## Priorities supported by the evidence

- For grep, reduce false candidates reaching fresh-file verification before tuning the substring matcher or candidate sorting.
- Validate four-reader behavior with the normal acceptance workload before changing the default.
- For filename search, measure bitmap survivors and candidate enumeration separately, then test stronger query-time filters without changing contiguous-match semantics.
- Measure scoped query parsing separately before adopting a canonicalization cache.
- Treat worker hot-loop CPU usage as a separate power/CPU concern, not a proven latency fix.

No new tests or production changes are needed to report these findings.

## Local evidence

- `benchmarks/profiles/fsearch-gap-95a48cf/`: exported profile XML, folded stacks, summaries, workload snapshots, and the executable-hash manifest.
- `benchmarks/results/fsearch-gap-95a48cf/`: per-pattern counters, timing diagnostics, response-parity summary, harness source, and session-specific scripts.
- `/tmp/fsearch-gap-profile/`: original traces, debug executable, and temporary diagnostic builds.

Both repository implementations remain unchanged by the investigation.
Scripts use session-specific paths and are not a portable benchmark package.
