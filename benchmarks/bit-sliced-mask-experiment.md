# Bit-sliced filename candidate masks

## Finding

A temporary bit-sliced mask prototype consistently reduced isolated search-engine median latency on Linux and Chromium. Socket round-trip results were inconsistent, so this is not yet a verified end-to-end improvement. Production source remains unchanged by this experiment.

All measurements used three rounds per invocation, 1,500 filename queries per round, a 50-result limit, and warmed queries. The baseline includes the retained eight-jobs-per-worker change. No content-search speedup is claimed.

## Algorithm

Currently, `fit_bits` reads each name's 64-bit character/start mask, tests the query, and packs 64 byte-sized decisions into a candidate bitmap.

The prototype transposes each block of 64 masks into 64 bit planes. Plane `b` contains one bit for each name containing mask bit `b`. Query evaluation intersects planes for required character bits and unions start-character planes. It tracks both zero missing bits and exactly one missing bit to preserve the current loose-mask typo candidate rule. Existing matching, typo scoring, ranking, and total counting remain unchanged.

This reduces the mask data read for selective queries. The prototype retains the original masks for candidate verification and adds a second representation.

## Isolated engine results

A temporary C++ executable called `Engine::search` directly, excluding JSON, IPC, client parsing, query parsing, and bit-plane construction. Each comparison used the same immutable index and queries. Name-only Chromium indexing avoided unrelated symbol/content-index startup work.

| Dataset | Entries | Run order | Baseline median | Prototype median | Reduction |
| --- | --- | --- | --- | --- | --- |
| Linux | 102,369 | Baseline, prototype | 18.250 µs | 15.000 µs | 17.8% |
| Linux | 102,369 | Prototype, baseline | 19.791 µs | 14.000 µs | 29.3% |
| Chromium | 555,626 | Baseline, prototype | 100.917 µs | 70.583 µs | 30.1% |
| Chromium | 555,626 | Prototype, baseline | 89.750 µs | 67.959 µs | 24.3% |

Rolling digests covering result totals, ordered entry IDs, and scores matched between variants for all these runs. This checks the benchmark workloads, not arbitrary query semantics or lifecycle behavior.

## Socket round-trip results

A temporary daemon executable linked the prototype search object into the normal executable. An isolated Python runner measured wall time and the daemon's reported search time separately. Server timings are rounded to 0.001 ms by the existing protocol.

| Linux run order | Baseline wall median | Prototype wall median | Baseline search median | Prototype search median |
| --- | --- | --- | --- | --- |
| Baseline, prototype | 50.625 µs | 44.625 µs | 21 µs | 14 µs |
| Prototype, baseline | 49.688 µs | 53.458 µs | 20 µs | 18 µs |

The first comparison improved wall median 11.9%; the reversed comparison regressed 7.6%. Non-search overhead was also higher in the reversed prototype run. The cause was not measured. Do not attribute the variation to a specific system condition or claim a reliable wall-latency win.

SHA-256 digests of complete responses excluding only the timing field matched across these runs.

## Memory and lifecycle costs

Added bit-plane payload was 619,520 bytes (0.59 MiB) on Linux and 3,365,376 bytes (3.21 MiB) on Chromium. These are representation sizes, not measured RSS deltas. Construction time and peak allocation were not measured; construction happened before timed queries.

The temporary prototype uses a process-global cache and is unsuitable for production. A production implementation must tie planes to immutable index ownership, initialize them before publishing an index, cover `each_match` as well as ranked search, and handle refreshes and concurrent readers safely. Overlay entries should retain their existing scoring path. Initial-query and rebuild latency require separate measurements.

Potentially replacing rather than duplicating raw masks could reduce memory, but matching currently uses the original mask for each surviving name. Reconstructing masks may offset the latency benefit. That alternative was not tested.

## Other options tested

- Two jobs per worker instead of eight: Linux wall median 49.438 µs versus 49.917 µs baseline. Too small to retain from one comparison.
- One job per worker: 51.292 µs, worse than baseline.
- Pack candidate bytes eight at a time using multiplication: 49.583 µs. No convincing wall gain; reverted.
- Avoid temporary number strings in filename JSON serialization: 49.875 µs. No convincing wall gain; reverted.
- Four search threads rather than eight: Linux wall median improved from 49.917 to 41.500 µs, then from 49.292 to 45.063 µs in reversed order. However, Chromium engine median regressed from 97-102 to 141 µs. Do not change the universal default based on the small dataset.

## Next implementation experiment

Implement immutable per-index bit planes, preserving the raw masks initially. Add a necessary semantic regression check for the candidate evaluator, validate complete responses for both datasets, and repeat three-round end-to-end comparisons in both orders. Measure plane construction and daemon RSS separately. Retain the change only if the metric that matters, socket query median, improves reproducibly.

Temporary prototype files are `/tmp/prepare-bitplane-prototype.py`, `/tmp/search-bitplanes.cpp`, `/tmp/fplussearch-engine-profile.cpp`, and `/tmp/fplussearch-profile.py`. Raw wall measurements are `/tmp/latency-bitplanes-wall-{before,after,before-repeat,after-repeat}.json`. These files are temporary and are not committed experiment infrastructure.
