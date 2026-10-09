# fplussearch versus fsearch

On this M4 Pro with 24 GiB RAM, fplussearch has lower median latency in both datasets.
Name search is about 2× faster; content search is about 2.1-2.4× faster.

## Results

Times are median socket round-trip milliseconds, including JSON serialization and client parsing.
The filename workload includes 300 exact names and 1,200 typos per dataset.
The content workload includes 67 literal patterns per dataset.

| Dataset | Operation | fplussearch | fsearch | fplussearch speedup |
|---|---|---:|---:|---:|
| Linux, 95,939 visible files | Name search | 0.082 ms | 0.162 ms | 1.98× |
| Linux | Content search | 1.114 ms | 2.621 ms | 2.35× |
| Chromium, 507,057 visible files | Name search | 0.201 ms | 0.432 ms | 2.14× |
| Chromium | Content search | 5.997 ms | 12.314 ms | 2.05× |

| Dataset | Accuracy | fplussearch | fsearch |
|---|---|---:|---:|
| Linux | Exact filename ranked first | 100% | 100% |
| Linux | Typo target ranked first | 98.75% | 97.75% |
| Chromium | Exact filename ranked first | 100% | 100% |
| Chromium | Typo target ranked first | 99.67% | 99.17% |

Across all content patterns, fplussearch returns 97,758 file-pattern pairs on Linux versus fsearch's 94,145.
On Chromium, it returns 217,994 versus 198,567.
Every fsearch file-pattern pair also appears in fplussearch's results.
These counts include repeated files across different patterns; they aren't unique file counts or independently verified recall.

[Raw results](results.json) include p90 latency, queries, patterns, coverage counts, machine details, and source revisions.

## Method

The workload adapts `../fsearch/demo/vs_fff.py`, using random seed 1 and sorted file enumeration.
It selects filenames unique among files listed by `fd`, then generates swap, deletion, insertion, and substitution typos.
Typos never alter the first character, matching the original benchmark's restriction.
Content patterns contain 60 sampled source identifiers and seven fixed patterns.
The precise queries differ from the published benchmark because its original file enumeration wasn't sorted.

Both engines use fresh, folder-local indexes of the same checkout.
fplussearch runs its production daemon from `build/fplussearch`.
fsearch uses its current Rust library through [a socket harness](fsearch-harness.rs), because its daemon always indexes `/`.
The harness preserves absolute paths with ancestor directory listings and follows fsearch's content-segment merge policy.
It uses fsearch's user-interactive name-search thread priority and existing dependency lockfile.

Requests alternate which engine runs first.
Each engine returns up to 50 results, with one matching line per file and no content-search time budget.
Separate unlimited-result requests compare content coverage.
Measurements start after both content indexes finish and one warmup request per operation completes.

These results describe one workload pass, not confidence intervals or cold-cache performance.
Different content eligibility policies remain active; content comparisons don't force identical indexed files.
The Rust harness isn't the complete production daemon, so these aren't production-daemon comparisons.
Name responses from the Rust harness include paths only; fplussearch also returns metadata.
Startup time, indexing throughput, memory use, and update latency aren't compared.
Existing user daemons remain running and can introduce background noise.
No application source files change.

## Run the benchmark

From the fplussearch repository, run:

```sh
python3 benchmarks/compare.py ~/fplussearch-bench --fsearch ../fsearch
```

You need macOS on Apple Silicon, the project's C++ build dependencies, Rust, Python 3, and `fd`.
The dataset directory must contain `linux` and `chromium` source trees.
The script builds both current engines, starts isolated benchmark processes, and writes `benchmarks/results.json`.
Temporary indexes and processes are removed after the run; existing daemons aren't restarted.

Verification: both datasets completed, filename sanity checks passed, and unlimited coverage requests completed.
The existing daemon-disconnect regression check also passes.
