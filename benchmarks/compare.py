"""Compare current fplussearch and fsearch on folder-local indexes.

Usage: python3 benchmarks/compare.py ~/fplussearch-bench --fsearch ../fsearch
Reuses the seeded filename/typo/content workload from fsearch/demo/vs_fff.py.
Starts only isolated benchmark processes; leaves existing daemons alone.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import re
import socket
import statistics
import subprocess
import tempfile
import time


def stats(values):
    values = sorted(values)
    return {"p50": statistics.median(values), "p90": values[int(len(values) * .9)], "mean": statistics.mean(values), "total": sum(values), "n": len(values)}


def workload(root):
    rng = random.Random(1)
    files = {}
    raw = subprocess.run(["fd", "-t", "f", "-0", ".", str(root)], capture_output=True, check=True).stdout
    for path in sorted(raw.decode().split("\0")[:-1]):
        files.setdefault(os.path.basename(path), []).append(path)
    pool = sorted(n for n, ps in files.items() if len(ps) == 1 and 6 <= len(n) <= 32 and n.isascii() and " " not in n and sum(c.isalpha() for c in n) >= 5)
    targets = rng.sample(pool, 300)
    queries = [(t, t) for t in targets]
    for target in targets:
        for kind in ("swap", "drop", "extra", "subst"):
            chars = list(target)
            letters = [i for i in range(1, len(chars)) if chars[i].isalpha()]
            pos = rng.choice(letters)
            if kind == "swap":
                pairs = [i for i in letters if i + 1 < len(chars) and chars[i + 1].isalpha() and chars[i] != chars[i + 1]]
                if not pairs:
                    continue
                pos = rng.choice(pairs)
                chars[pos], chars[pos + 1] = chars[pos + 1], chars[pos]
            elif kind == "drop":
                del chars[pos]
            elif kind == "extra":
                chars.insert(pos, rng.choice("abcdefghijklmnopqrstuvwxyz"))
            else:
                chars[pos] = rng.choice([c for c in "abcdefghijklmnopqrstuvwxyz" if c != chars[pos].lower()])
            queries.append(("".join(chars), target))
    sources = [p for ps in files.values() for p in ps if p.endswith((".c", ".h", ".rs", ".ts", ".tsx", ".js", ".py", ".go", ".swift"))]
    identifiers = set()
    for _ in range(10000):
        if len(identifiers) == 60:
            break
        words = re.findall(r"\b[a-z][a-z0-9_]{9,30}\b", Path(rng.choice(sources)).read_text(errors="ignore"))
        if words:
            identifiers.add(rng.choice(words))
    if len(identifiers) != 60:
        raise RuntimeError("could not select 60 content patterns")
    return len(raw.split(b"\0")) - 1, queries, sorted(identifiers) + ["mutex_lock", "kmalloc", "EXPORT_SYMBOL_GPL", "spin_lock_irqsave", "struct device", "TODO", "return -EINVAL"]


class Client:
    def __init__(self, path):
        self.sock = socket.socket(socket.AF_UNIX)
        self.sock.settimeout(600)
        self.sock.connect(str(path))
        self.conn = self.sock.makefile("rw")

    def call(self, req):
        start = time.perf_counter_ns()
        self.conn.write(json.dumps(req) + "\n")
        self.conn.flush()
        result = json.loads(self.conn.readline())
        elapsed = (time.perf_counter_ns() - start) / 1e6
        if not result.get("ok"):
            raise RuntimeError(result)
        return elapsed, result

    def close(self):
        self.conn.close()
        self.sock.close()


class Usage(ctypes.Structure):
    _fields_ = [("uuid", ctypes.c_uint8 * 16)] + [(name, ctypes.c_uint64) for name in (
        "user_time", "system_time", "pkg_idle_wkups", "interrupt_wkups", "pageins",
        "wired_size", "resident_size", "phys_footprint", "proc_start_abstime",
        "proc_exit_abstime", "child_user_time", "child_system_time", "child_pkg_idle_wkups",
        "child_interrupt_wkups", "child_pageins", "child_elapsed_abstime",
        "diskio_bytesread", "diskio_byteswritten")]


libproc = ctypes.CDLL("/usr/lib/libproc.dylib", use_errno=True)
libproc.proc_pid_rusage.argtypes = [ctypes.c_int, ctypes.c_int, ctypes.c_void_p]
libproc.proc_pid_rusage.restype = ctypes.c_int


def memory(pid):
    usage = Usage()
    if libproc.proc_pid_rusage(pid, 2, ctypes.byref(usage)) != 0:
        raise OSError(ctypes.get_errno(), "proc_pid_rusage")
    return {"footprint_mib": usage.phys_footprint / 2**20, "resident_mib": usage.resident_size / 2**20}


def run(root, harness, temp, baseline=None, rounds=1, only=None):
    count, queries, patterns = workload(root)
    procs, logs, clients, processes = [], [], {}, {}
    start = time.monotonic()
    try:
        home = temp / "home"
        (home / "Library/Caches/fplussearch").mkdir(parents=True)
        rustdir = temp / "rust"
        launches = [
            ("fplussearch", [str(Path("build/fplussearch").resolve()), "serve", "--root", str(root)], {**os.environ, "HOME": str(home)}),
            ("fsearch", [str(harness), str(root), str(rustdir)], os.environ),
        ]
        sockets = [("fplussearch", home / "Library/Caches/fplussearch", "*.sock"), ("fsearch", rustdir, "search.sock")]
        if baseline:
            baseline_home = temp / "baseline-home"
            (baseline_home / "Library/Caches/fplussearch").mkdir(parents=True)
            launches.append(("baseline", [str(baseline), "serve", "--root", str(root)], {**os.environ, "HOME": str(baseline_home)}))
            sockets.append(("baseline", baseline_home / "Library/Caches/fplussearch", "*.sock"))
        if only:
            launches = [entry for entry in launches if entry[0] == only]
            sockets = [entry for entry in sockets if entry[0] == only]
        compare_baseline = baseline is not None and only is None
        for name, cmd, env in launches:
            log = open(temp / f"{name}.log", "w")
            logs.append(log)
            process = subprocess.Popen(cmd, stdout=log, stderr=log, env=env)
            procs.append(process)
            processes[name] = process
        while len(clients) < len(launches):
            if time.monotonic() - start > 1200:
                raise TimeoutError("index build exceeded 20 minutes")
            for name, directory, pattern in sockets:
                if name not in clients:
                    for path in directory.glob(pattern):
                        try:
                            clients[name] = Client(path)
                        except (FileNotFoundError, ConnectionRefusedError):
                            pass
            for process in procs:
                if process.poll() is not None:
                    raise RuntimeError(f"benchmark process exited: logs in {temp}")
            time.sleep(.1)
        while True:
            status = {name: client.call({"op": "status"})[1] for name, client in clients.items()}
            if all(s.get("entries", 0) and s.get("content_docs", 0) > 0 and s.get("ready", True) and not s.get("busy", False) and not s.get("content_pending", True) for s in status.values()):
                break
            if time.monotonic() - start > 1200:
                raise TimeoutError("content build exceeded 20 minutes")
            time.sleep(.5)
        ready_memory = {n: memory(p.pid) for n, p in processes.items()}
        peak_memory = {n: {key: 0 for key in m} for n, m in ready_memory.items()}
        def sample_memory():
            for n, p in processes.items():
                for key, value in memory(p.pid).items():
                    peak_memory[n][key] = max(peak_memory[n][key], value)
        result = {"root": str(root), "visible_files": count, "queries": queries, "patterns": patterns, "status": status, "rounds": rounds}
        timings = {n: {"name": [], "exact": [], "typo": [], "grep": []} for n in clients}
        scores = {n: {"exact": [0, 0], "typo": [0, 0]} for n in clients}
        for client in clients.values():
            _, check = client.call({"q": f"in:{root} {queries[0][0]}", "limit": 50})
            if not any(os.path.basename(h["path"]) == queries[0][1] for h in check["hits"]):
                raise RuntimeError("harness sanity check failed: known filename missing")
            if not all(s.get("content_docs", 0) > 0 for s in status.values()):
                raise RuntimeError("harness sanity check failed: empty content index")
            client.call({"q": f"in:{root} warmup", "limit": 50})
            client.call({"op": "grep", "q": f"in:{root}", "pattern": "warmup", "limit": 50, "per_file": 1, "budget_ms": 0})
        names = [n for n in ["fsearch", "fplussearch", "baseline"] if n in clients]
        name_digests = {n: hashlib.sha256() for n in names}
        content_digests = {n: hashlib.sha256() for n in names}
        round_timings = []
        name_parity = True
        for i, (query, target) in enumerate(queries * rounds):
            kind = "exact" if query == target else "typo"
            responses = {}
            order = names[i % len(names):] + names[:i % len(names)]
            if i % 2:
                order = order[::-1]
            for name in order:
                ms, response = clients[name].call({"q": f"in:{root} {query}", "limit": 50})
                responses[name] = response
                name_digests[name].update(json.dumps(response["hits"], sort_keys=True).encode())
                timings[name]["name"].append(ms)
                timings[name][kind].append(ms)
                hits = response["hits"]
                scores[name][kind][0] += bool(hits) and os.path.basename(hits[0]["path"]) == target
                scores[name][kind][1] += 1
            if compare_baseline:
                name_parity &= responses["baseline"]["hits"] == responses["fplussearch"]["hits"]
            sample_memory()
        coverage = []
        for i, pattern in enumerate(patterns * rounds):
            req = {"op": "grep", "q": f"in:{root}", "pattern": pattern, "mode": "literal", "limit": 50, "per_file": 1, "budget_ms": 0}
            order = names[i % len(names):] + names[:i % len(names)]
            if i % 2:
                order = order[::-1]
            for name in order:
                ms, _ = clients[name].call(req)
                timings[name]["grep"].append(ms)
            sample_memory()
            if i >= len(patterns):
                continue
            sets = {}
            for name in names[::1 if i % 2 else -1]:
                _, response = clients[name].call({**req, "limit": 1000000})
                if not response["complete"]:
                    raise RuntimeError(f"incomplete coverage: {name} {pattern}")
                sets[name] = {f["path"] for f in response["files"]}
                content_digests[name].update(json.dumps(sorted(response["files"], key=lambda f: f["path"]), sort_keys=True).encode())
                sample_memory()
            if compare_baseline and sets["baseline"] != sets["fplussearch"]:
                raise RuntimeError(f"content parity failed: {pattern}")
            shared = {"both": len(sets[names[0]] & sets[names[1]])} if len(names) > 1 else {}
            coverage.append({"pattern": pattern, **{n: len(s) for n, s in sets.items()}, **shared})
        if not name_parity:
            raise RuntimeError("ordered filename response parity failed")
        for round_index in range(rounds):
            round_timings.append({n: {k + "_ms": stats(timings[n][k][round_index * size:(round_index + 1) * size]) for k, size in (("name", len(queries)), ("grep", len(patterns)))} for n in names})
        result["round_results"] = round_timings
        result["pattern_timings"] = {p: {n: stats(timings[n]["grep"][i::len(patterns)]) for n in names} for i, p in enumerate(patterns)}
        result["memory"] = {n: {"ready": ready_memory[n], "max_observed_after_request": peak_memory[n], "after": memory(processes[n].pid)} for n in names}
        result["baseline_parity"] = name_parity if compare_baseline else None
        result["name_results_digest"] = {n: digest.hexdigest() for n, digest in name_digests.items()}
        result["content_results_digest"] = {n: digest.hexdigest() for n, digest in content_digests.items()}
        result["coverage"] = coverage
        result["results"] = {n: {**{k + "_ms": stats(v) for k, v in timings[n].items()}, **{k + "_top1": a / b for k, (a, b) in scores[n].items()}} for n in names}
        return result
    finally:
        for client in clients.values():
            client.close()
        for process in procs:
            if process.poll() is None:
                process.terminate()
                try:
                    process.wait(timeout=10)
                except subprocess.TimeoutExpired:
                    process.kill()
                    process.wait()
        for log in logs:
            log.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("datasets", type=Path)
    parser.add_argument("--fsearch", type=Path, default=Path("../fsearch"))
    parser.add_argument("--out", type=Path, default=Path("benchmarks/results.json"))
    parser.add_argument("--baseline", type=Path)
    parser.add_argument("--rounds", type=int, default=1)
    parser.add_argument("--isolate", action=argparse.BooleanOptionalAction, default=True)
    parser.add_argument("--reverse", action="store_true")
    args = parser.parse_args()
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    if args.baseline:
        args.baseline = args.baseline.resolve()
    subprocess.run(["make", "-j8"], check=True)
    with tempfile.TemporaryDirectory(prefix="fbench-", dir="/tmp") as tmp:
        temp = Path(tmp)
        crate = temp / "harness"
        (crate / "src").mkdir(parents=True)
        manifest = '[package]\nname="fsearch-benchmark"\nversion="0.1.0"\nedition="2024"\n[dependencies]\nfsearch={path=' + json.dumps(str(args.fsearch.resolve())) + '}\nserde_json="1"\nrayon="1"\nlibc="0.2"\n'
        source_manifest = (args.fsearch / "Cargo.toml").read_text()
        release_profile = re.search(r"(?ms)^\[profile\.release\]\n.*?(?=^\[|\Z)", source_manifest)
        if release_profile is None:
            raise RuntimeError("fsearch release profile missing")
        (crate / "Cargo.toml").write_text(manifest + release_profile.group(0))
        (crate / "Cargo.lock").write_text((args.fsearch / "Cargo.lock").read_text())
        source_main = (args.fsearch / "src/main.rs").read_text()
        allocator = re.search(r"(?ms)^struct Alloc;.*?(?=^const USAGE:)", source_main)
        if allocator is None:
            raise RuntimeError("fsearch production allocator missing")
        (crate / "src/main.rs").write_text("use std::alloc::{GlobalAlloc, Layout, System};\n" + allocator.group(0) + Path("benchmarks/fsearch-harness.rs").read_text())
        subprocess.run(["cargo", "build", "--release", "--manifest-path", str(crate / "Cargo.toml")], check=True)
        output = {"method": "folder-local indexes; Unix socket round-trip; seed 1; 50 results; unlimited content budget; " + ("one engine process at a time" if args.isolate else "concurrent processes; rotating request order"), "commits": {n: subprocess.check_output(["git", "-C", p, "rev-parse", "HEAD"], text=True).strip() for n, p in (("fplussearch", "."), ("fsearch", str(args.fsearch)))}, "machine": subprocess.check_output(["sysctl", "hw.model", "hw.memsize", "hw.ncpu", "machdep.cpu.brand_string"], text=True).strip(), "datasets": {}}
        for name in ("linux", "chromium"):
            directory = temp / name
            directory.mkdir()
            root = (args.datasets / name).resolve()
            harness = crate / "target/release/fsearch-benchmark"
            if args.isolate:
                order = ["baseline", "fplussearch", "fsearch"] if args.baseline else ["fplussearch", "fsearch"]
                if args.reverse:
                    order.reverse()
                runs = {}
                for variant in order:
                    variant_dir = directory / variant
                    variant_dir.mkdir()
                    runs[variant] = run(root, harness, variant_dir, args.baseline, args.rounds, variant)
                    print(name, variant, json.dumps(runs[variant]["results"][variant]), flush=True)
                merged = dict(runs["fplussearch"])
                for key in ("status", "results", "memory", "name_results_digest", "content_results_digest"):
                    merged[key] = {n: runs[n][key][n] for n in runs}
                merged["round_results"] = [{n: runs[n]["round_results"][i][n] for n in runs} for i in range(args.rounds)]
                merged["pattern_timings"] = {p: {n: runs[n]["pattern_timings"][p][n] for n in runs} for p in merged["patterns"]}
                merged["coverage"] = [{"pattern": p, **{n: runs[n]["coverage"][i][n] for n in runs}} for i, p in enumerate(merged["patterns"])]
                if args.baseline:
                    for key in ("name_results_digest", "content_results_digest"):
                        if merged[key]["baseline"] != merged[key]["fplussearch"]:
                            raise RuntimeError(f"isolated baseline parity failed: {key}")
                    merged["baseline_parity"] = True
                output["datasets"][name] = merged
            else:
                output["datasets"][name] = run(root, harness, directory, args.baseline, args.rounds)
            args.out.write_text(json.dumps(output, indent=2) + "\n")
            print(name, json.dumps(output["datasets"][name]["results"], indent=2), flush=True)


if __name__ == "__main__":
    main()
