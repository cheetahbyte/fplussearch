# Query filter-plan experiment

## Decision

Reject the prototype. Precomputing mask-plane indices and specializing exact filtering did not produce a repeatable filename latency improvement.
The production implementation remains the index-owned bit-plane evaluator.

## Experiment

The prototype builds one plan per candidate token when constructing `Scan`.
Each plan stores required, typo-eligible, and start-plane indices.
Required-plane intersection runs first and stops on an empty candidate set.
Tokens without start planes use plain intersection; other tokens track at most one missing typo-eligible bit.

This removes repeated bit-index extraction and required-bit classification from each block.
It also introduces plan allocation, indexed arrays, and early-exit branches.
The measurements do not isolate which costs offset the savings.

## Measurements

Each invocation uses three rounds on an isolated daemon, replaying the saved Linux or Chromium workload.
Each invocation contains 4,500 filename requests and 201 content requests.
Baseline executables contain the accepted bit-plane change, not the original pre-bit-plane implementation.

| Dataset | Baseline filename median | Prototype median | Repeated baseline | Repeated prototype |
| --- | ---: | ---: | ---: | ---: |
| Linux | 67.625 µs | 67.750 µs | 66.438 µs | 66.417 µs |
| Chromium | 105.375 µs | 103.813 µs | 104.854 µs | 105.229 µs |

Initial order: baseline Linux, baseline Chromium, prototype Linux, prototype Chromium.
Repeated order: prototype Chromium, prototype Linux, baseline Chromium, baseline Linux.
Linux is flat or slightly slower. Chromium's initial 1.5% improvement reverses to a small regression.

Content medians:

| Dataset | Baseline | Prototype | Repeated baseline | Repeated prototype |
| --- | ---: | ---: | ---: | ---: |
| Linux | 1.2001 ms | 1.2392 ms | 1.4483 ms | 1.2437 ms |
| Chromium | 3.2844 ms | 3.4174 ms | 3.3170 ms | 3.3788 ms |

Content timing varies; no content algorithm changed, and no content improvement is claimed.
Raw measurements are ignored files named `benchmarks/filterplan-{linux,chromium}-{before,after,before-repeat,after-repeat}.json`.

## Verification

The prototype passes existing C++ search-matching checks.
A temporary check compares its production evaluator against `Token::fits` for 650,000 candidate bitmaps.
Temporary lifecycle checks pass for build, load, refresh, retained snapshots, memory fallback, ranked search, and the shared name scan.
Exact-name and typo top-result accuracy remain unchanged across all timed runs.
After restoring the production evaluator, the optimized build, matching checks, and whitespace checks pass again.

Only the experiment's changes were removed. Existing bit-plane changes and earlier working-tree edits remain intact.
The next proposed experiment targets substring matching, which dominates Chromium's remaining filename query work.
