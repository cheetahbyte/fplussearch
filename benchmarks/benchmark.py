"""Benchmark the normal fplussearch binary with fsearch's vs_fff workload."""

import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import time

from common import Client, stats, workload


def measure(client, root, queries, patterns, rounds):
    scope = f'in:{json.dumps(str(root))}'
    client.call({"q": f"{scope} warmup", "limit": 50})
    client.call({"op": "grep", "q": scope, "pattern": "warmup", "mode": "literal", "limit": 50, "per_file": 1, "budget_ms": 0})
    name_times, content_times = [], []
    accuracy = {"exact": [0, 0], "typo": [0, 0]}
    for _ in range(rounds):
        for query, target in queries:
            elapsed, response = client.call({"q": f"{scope} {query}", "limit": 50})
            name_times.append(elapsed)
            score = accuracy["exact" if query == target else "typo"]
            score[0] += bool(response["hits"]) and Path(response["hits"][0]["path"]).name == target
            score[1] += 1
    for _ in range(rounds):
        for pattern in patterns:
            elapsed, response = client.call({"op": "grep", "q": scope, "pattern": pattern, "mode": "literal", "limit": 50, "per_file": 1, "budget_ms": 0})
            if not response["complete"]:
                raise RuntimeError(f"incomplete content query: {pattern}")
            content_times.append(elapsed)
    return {"name_ms": stats(name_times), "grep_ms": stats(content_times), **{f"{kind}_top1": correct / count if count else None for kind, (correct, count) in accuracy.items()}}


def run(binary, root, queries, patterns, rounds):
    with tempfile.TemporaryDirectory(prefix="fpbench-", dir="/tmp") as directory:
        home = Path(directory)
        cache = home / "Library/Caches/fplussearch"
        cache.mkdir(parents=True)
        with (home / "daemon.log").open("w+") as log:
            process = subprocess.Popen([str(binary), "serve", "--root", str(root)], env={**os.environ, "HOME": str(home)}, stdout=log, stderr=log)
            client = None
            try:
                deadline = time.monotonic() + 1200
                status = None
                while time.monotonic() < deadline:
                    if process.poll() is not None:
                        log.seek(0)
                        raise RuntimeError(f"benchmark daemon exited: {log.read()[-4000:]}")
                    sockets = list(cache.glob("*.sock"))
                    if client is None and sockets:
                        try:
                            client = Client(sockets[0])
                        except (ConnectionRefusedError, FileNotFoundError):
                            pass
                    if client is not None:
                        _, status = client.call({"op": "status"})
                        if status["ready"] and not status["busy"] and not status["content_pending"]:
                            break
                    time.sleep(.2)
                else:
                    raise RuntimeError("benchmark daemon readiness timed out")
                return measure(client, root, queries, patterns, rounds), status
            finally:
                if client is not None:
                    client.close()
                if process.poll() is None:
                    process.terminate()
                    try:
                        process.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        process.kill()
                        process.wait()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("folder", type=Path)
    parser.add_argument("--binary", type=Path, default=Path("build/fplussearch"), help="existing executable; this runner never builds it")
    parser.add_argument("--rounds", type=int, default=5)
    parser.add_argument("--out", type=Path, default=Path("benchmarks/fplussearch-results.json"))
    parser.add_argument("--workload", type=Path, help="replay the workload from a previous benchmark JSON file")
    args = parser.parse_args()
    root, binary = args.folder.resolve(), args.binary.resolve()
    if not root.is_dir():
        parser.error("folder must be an existing source tree")
    if not binary.is_file() or not os.access(binary, os.X_OK):
        parser.error("--binary must be an existing executable; build fplussearch first")
    if args.rounds < 1:
        parser.error("--rounds must be positive")
    if args.workload:
        saved = json.loads(args.workload.read_text())
        if saved["root"] != str(root):
            parser.error("workload belongs to a different folder")
        count, queries, patterns = saved["folder_files"], saved["workload"]["queries"], saved["workload"]["patterns"]
    else:
        count, queries, patterns = workload(root, sort_files=False)
    results, status = run(binary, root, queries, patterns, args.rounds)
    output = {"root": str(root), "folder_files": count, "fplussearch": results, "patterns": len(patterns), "queries": len(queries), "rounds": args.rounds, "workload": {"queries": queries, "patterns": patterns}, "status": status, "binary": {"path": str(binary), "sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}, "method": "fsearch vs_fff workload; seed 1; fd enumeration order; JSON-lines socket round-trip; 50 results; literal content; one match per file; unlimited content budget; fresh folder-local index; warmed queries"}
    args.out.parent.mkdir(parents=True, exist_ok=True)
    args.out.write_text(json.dumps(output, indent=2) + "\n")
    print("Median query latency (ms)")
    print(f"Filename: {results['name_ms']['p50']:.4f}")
    print(f"Content:  {results['grep_ms']['p50']:.4f}")
    print(f"Raw results: {args.out}")


if __name__ == "__main__":
    main()
