# CPU profiling tools

These tools profile the existing search implementation without changing production code. They require macOS, Xcode's `xctrace`, Python, and the project's existing PCRE2 dependency.

## Build with symbols

Run from the repository root:

```sh
make -f benchmarks/profiling/Makefile -j8
```

This creates `build/profiling/fplussearch`, `build/profiling/replay`, and their debug-symbol bundles. Compilation keeps `-O3 -mcpu=native` and adds `-g`. It does not replace `build/fplussearch`.

## Profile the daemon

Replace `WORKLOAD.json` with an existing standalone benchmark result containing its replayable workload:

```sh
python3 -B benchmarks/profiling/socket_replay.py \
  build/profiling/fplussearch WORKLOAD.json /tmp/fplussearch-profile
```

The runner creates an isolated daemon and waits for filename, symbol, and content indexing to finish. It records filename and content requests separately. Each recording lasts 12 seconds, with requests replayed for 14 seconds. These are sampling runs, not latency benchmarks. Latency benchmarks remain limited to three rounds.

Only the runner's own daemon and recorder are stopped. Existing daemons are untouched. Trace files contain process paths and system metadata; inspect them before sharing.

## Profile the engine

Replace `INDEX.bin` with a compatible filename index. Replace `QUERIES.txt` with one query per line:

```sh
xctrace record --template 'Time Profiler' --time-limit 15s \
  --output /tmp/engine.trace --no-prompt --launch -- \
  build/profiling/replay INDEX.bin QUERIES.txt 8 12
```

The replay loads the index without modifying it, parses queries, and warms 20 queries before continuous replay. It omits daemon request parsing, response serialization, and the socket. An incompatible index fails rather than rebuilding it.

The capture in this session used `/tmp/latency-chromium-names.bin` and `/tmp/chromium-queries.txt`. Those are temporary files.

## Export stacks

Export the native recording and exclude its startup interval:

```sh
xctrace export --input /tmp/engine.trace \
  --xpath '/trace-toc/run[@number="1"]/data/table[@schema="time-profile"]' \
  --output /tmp/engine.xml
python3 -B benchmarks/profiling/export_stacks.py \
  /tmp/engine.xml benchmarks/profiles/engine --start 1 --end 12
```

The exporter resolves XML references, retains running samples, reverses leaf-first backtraces, and sums their weights in nanoseconds. It simplifies template names for readability. `all.folded` contains every resolved running stack. `query.folded` retains stacks containing filename search or content grep. `summary.json` includes self and inclusive percentages. Inclusive percentages overlap and must not be added.

The converter targets the seven-column `time-profile` schema exported by this installed Xcode version. It is not a general converter for every Instruments schema.

## Render a flamegraph

Use the upstream [FlameGraph renderer](https://github.com/brendangregg/FlameGraph):

```sh
curl -fsSL https://raw.githubusercontent.com/brendangregg/FlameGraph/master/flamegraph.pl \
  -o /tmp/flamegraph.pl
perl /tmp/flamegraph.pl --countname nanoseconds --hash \
  benchmarks/profiles/engine/all.folded > benchmarks/profiles/engine/all.svg
```

SVG files are self-contained and interactive. Open one in a browser, select a frame to zoom, and use **Search** to find a function. No application is opened automatically.

## Interpret results

Flamegraph width represents sampled CPU weight, not individual request latency. Long or expensive queries contribute more width. User stacks ending at a syscall do not reveal kernel internals or off-CPU waits.

Use an unprofiled three-round benchmark to accept a change. Profiling perturbs scheduling and cannot establish a latency improvement itself. The recorded findings and flamegraphs are in [the profiling report](../profiles/README.md).
