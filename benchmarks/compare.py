"""Compare current fplussearch and fsearch on folder-local indexes.

Usage: python3 benchmarks/compare.py ~/fplussearch-bench --fsearch ../fsearch
Reuses the seeded filename/typo/content workload from fsearch/demo/vs_fff.py.
Starts only isolated benchmark processes; leaves existing daemons alone.
"""
import argparse
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
    return {"p50": statistics.median(values), "p90": values[int(len(values) * .9)], "n": len(values)}


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


def run(root, harness, temp):
    count, queries, patterns = workload(root)
    procs, logs, clients = [], [], {}
    start = time.monotonic()
    try:
        home = temp / "home"
        (home / "Library/Caches/fplussearch").mkdir(parents=True)
        rustdir = temp / "rust"
        for name, cmd, env in (
            ("fplussearch", [str(Path("build/fplussearch").resolve()), "serve", "--root", str(root)], {**os.environ, "HOME": str(home)}),
            ("fsearch", [str(harness), str(root), str(rustdir)], os.environ),
        ):
            log = open(temp / f"{name}.log", "w")
            logs.append(log)
            procs.append(subprocess.Popen(cmd, stdout=log, stderr=log, env=env))
        while len(clients) < 2:
            if time.monotonic() - start > 1200:
                raise TimeoutError("index build exceeded 20 minutes")
            for name, directory, pattern in (("fplussearch", home / "Library/Caches/fplussearch", "*.sock"), ("fsearch", rustdir, "search.sock")):
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
            if all(s.get("entries", 0) and not s.get("content_pending", True) for s in status.values()):
                break
            if time.monotonic() - start > 1200:
                raise TimeoutError("content build exceeded 20 minutes")
            time.sleep(.5)
        result = {"root": str(root), "visible_files": count, "queries": queries, "patterns": patterns, "status": status}
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
        names = ["fsearch", "fplussearch"]
        for i, (query, target) in enumerate(queries):
            kind = "exact" if query == target else "typo"
            for name in names[::1 if i % 2 else -1]:
                ms, response = clients[name].call({"q": f"in:{root} {query}", "limit": 50})
                timings[name]["name"].append(ms)
                timings[name][kind].append(ms)
                hits = response["hits"]
                scores[name][kind][0] += bool(hits) and os.path.basename(hits[0]["path"]) == target
                scores[name][kind][1] += 1
        coverage = []
        for i, pattern in enumerate(patterns):
            req = {"op": "grep", "q": f"in:{root}", "pattern": pattern, "mode": "literal", "limit": 50, "per_file": 1, "budget_ms": 0}
            for name in names[::1 if i % 2 else -1]:
                ms, _ = clients[name].call(req)
                timings[name]["grep"].append(ms)
            sets = {}
            for name in names[::1 if i % 2 else -1]:
                _, response = clients[name].call({**req, "limit": 1000000})
                if not response["complete"]:
                    raise RuntimeError(f"incomplete coverage: {name} {pattern}")
                sets[name] = {f["path"] for f in response["files"]}
            coverage.append({"pattern": pattern, **{n: len(s) for n, s in sets.items()}, "both": len(sets[names[0]] & sets[names[1]])})
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
    args = parser.parse_args()
    subprocess.run(["make", "-j8"], check=True)
    with tempfile.TemporaryDirectory(prefix="fbench-", dir="/tmp") as tmp:
        temp = Path(tmp)
        crate = temp / "harness"
        (crate / "src").mkdir(parents=True)
        manifest = '[package]\nname="fsearch-benchmark"\nversion="0.1.0"\nedition="2024"\n[dependencies]\nfsearch={path=' + json.dumps(str(args.fsearch.resolve())) + '}\nserde_json="1"\nrayon="1"\nlibc="0.2"\n'
        (crate / "Cargo.toml").write_text(manifest)
        (crate / "Cargo.lock").write_text((args.fsearch / "Cargo.lock").read_text())
        (crate / "src/main.rs").write_text(Path("benchmarks/fsearch-harness.rs").read_text())
        subprocess.run(["cargo", "build", "--release", "--manifest-path", str(crate / "Cargo.toml")], check=True)
        output = {"method": "folder-local indexes; current source; Unix socket round-trip; seed 1; alternating order; 50 results; unlimited content budget", "commits": {n: subprocess.check_output(["git", "-C", p, "rev-parse", "HEAD"], text=True).strip() for n, p in (("fplussearch", "."), ("fsearch", str(args.fsearch)))}, "machine": subprocess.check_output(["sysctl", "hw.model", "hw.memsize", "hw.ncpu", "machdep.cpu.brand_string"], text=True).strip(), "datasets": {}}
        for name in ("linux", "chromium"):
            directory = temp / name
            directory.mkdir()
            output["datasets"][name] = run((args.datasets / name).resolve(), crate / "target/release/fsearch-benchmark", directory)
            args.out.write_text(json.dumps(output, indent=2) + "\n")
            print(name, json.dumps(output["datasets"][name]["results"], indent=2), flush=True)


if __name__ == "__main__":
    main()
