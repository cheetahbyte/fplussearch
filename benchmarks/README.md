# Benchmark fplussearch

[benchmark.py](benchmark.py) measures filename and content query latency using the normal fplussearch executable. It doesn't build or run fsearch, compile Rust, install dependencies, or alter existing daemons.

## Run

You need macOS on Apple Silicon, Python 3.9 or later, and `fd` on `PATH`. Build fplussearch using the [project build instructions](../README.md#build), then run from the repository root:

```sh
make -j8
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary build/fplussearch \
  --rounds 5 \
  --out benchmarks/linux.json

python3 benchmarks/benchmark.py ~/fplussearch-bench/chromium \
  --binary build/fplussearch \
  --rounds 5 \
  --out benchmarks/chromium.json
```

You can use another source tree with at least 300 eligible unique filenames and enough source identifiers. The runner accepts one folder per invocation and any existing fplussearch executable through `--binary`. Five rounds are the default.

It prints two medians in milliseconds: filename and content. Lower is better. JSON includes p50, p90, sample counts, exact-name and typo accuracy, the executable hash, and the full query workload. JSON files under `benchmarks/` aren't tracked by Git.

## Repeat the same workload

Capture a baseline before changing code:

```sh
make -j8
cp build/fplussearch /tmp/fplussearch-before
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary /tmp/fplussearch-before \
  --out benchmarks/linux-before.json
```

After changing and rebuilding fplussearch, replay the saved queries:

```sh
make -j8
python3 benchmarks/benchmark.py ~/fplussearch-bench/linux \
  --binary build/fplussearch \
  --workload benchmarks/linux-before.json \
  --out benchmarks/linux-after.json
```

Keep the dataset unchanged. Record its revision and the baseline source revision separately; executable hashes don't identify source revisions. Replay requires the same folder path and doesn't require `fd`.

## Compatibility with fsearch's benchmark

The workload follows `fsearch/demo/vs_fff.py`: random seed 1, 300 unique ASCII filenames, up to 1,200 letter typos, 60 sampled source identifiers, and seven fixed content patterns. It preserves `fd` enumeration order, rather than the sorted enumeration used by our older [comparison runner](compare.py).

Filename requests use `q` and `limit: 50`. Content requests use `op: grep`, literal mode, 50 results, one match per file, and no time budget. Responses expose `ok`, `hits[].path`, and `files[].path`. Status exposes `content_pending`. These are the request and response fields used by fsearch's harness.

The JSON uses the same `name_ms`, `grep_ms`, `exact_top1`, and `typo_top1` fields, under `fplussearch` instead of `fsearch`. It also records additional statistics and a replayable workload.

The upstream script itself isn't drop-in compatible: it hard-codes fsearch's binary and socket, imports `fff`, and stops its existing daemon. This runner uses the same search workload and protocol without those side effects. It builds a fresh folder-local index through the normal `fplussearch serve --root` command in a temporary home directory.

`fd` ordering can vary between invocations and affect sampled content identifiers. For exact before/after comparisons, use `--workload` instead of regenerating queries. Shared content eligibility and ranking aren't guaranteed across engines.

## Measurement limits

Timing includes Unix socket round trips, JSON serialization, and client parsing. Filename medians combine exact and typo queries. Results are warmed with one request per operation after content indexing finishes. Medians pool timed requests across all rounds.

These aren't cold-cache measurements or indexing benchmarks. The runner doesn't flush the OS cache or stop background processes. Only its own daemon is terminated, and temporary indexes are removed after the run.

The older three-engine runner remains available as [compare.py](compare.py), but isn't required for standalone benchmarking.
