# Median query latency experiment

## Retained change

`Engine::search` targets eight jobs per worker instead of 24. Other search paths and the worker pool remain unchanged.

Fewer jobs reduce scheduling operations and per-job result containers. Matching, scoring, and final result ordering are unchanged.

## Linux measurements

The standalone runner used a preserved baseline executable, an identical replayed workload, and three rounds per invocation. Times include the socket round trip.

| Run order | Baseline filename median | Changed filename median | Reduction |
| --- | --- | --- | --- |
| Changed, baseline | 0.0699 ms | 0.0522 ms | 25.3% |
| Baseline, changed | 0.0672 ms | 0.0512 ms | 23.8% |

The content search implementation is unchanged. Content medians varied from 0.9659 to 1.0871 ms across the repeated baseline and changed runs. This experiment does not establish a content improvement or exclude a process-level regression.

The build, search-matching regression checks, and daemon-disconnect test passed. Full response parity for the retained change was not measured.

## Rejected experiments

- Skip typo scorers when their score bounds cannot beat the current match. Gains were not consistent across run orders.
- Publish worker completion once per worker instead of once per job. Chromium filename results were inconsistent.
- Separate worker counters onto different cache lines. Chromium filename results were inconsistent.
- Claim four jobs at a time. Median filename latency regressed on both datasets.

These experiments were reverted.

## Limits and next measurements

Chromium validation timed out during standalone daemon readiness. Three rounds limit query repetition, not fresh content-index setup time.

Validate the job target on Chromium before treating it as a general improvement. Measure full response parity and daemon memory separately. No memory improvement is claimed.

Next investigate content verification: it reads candidate files and retains read buffers for the query. Measure candidate counts, bytes read, and posting-list evaluation time before adding a cache. A cache trades memory and invalidation complexity for latency.
