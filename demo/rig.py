"""Fast, persistent benchmark rig for fplussearch using socket/daemon mode.
Runs queries in a corpus against a binary, returns execution timings and result digests.
Mirrors fsearch's examples/rig.rs output format so demo/abba.py works seamlessly.

usage: python3 demo/rig.py <binary> <corpus.json> <out.json> [kinds]
kinds: comma-separated subset of name,typing,grep (default all)
"""

import hashlib
import json
import os
import socket
import subprocess
import sys
import tempfile
import time
from pathlib import Path


def fnv64(data: bytes, h: int = 0xcbf29ce484222325) -> int:
    for b in data:
        h = ((h ^ b) * 0x100000001b3) & 0xffffffffffffffff
    return h


class DaemonClient:
    def __init__(self, binary_path: str, root_path: str = "/"):
        self.tmp = tempfile.TemporaryDirectory(prefix="fpr-", dir="/tmp")
        self.home = Path(self.tmp.name)
        self.is_fsearch = "fsearch" in Path(binary_path).name and "fplussearch" not in Path(binary_path).name

        real_home = Path(os.environ.get("HOME", ""))
        if self.is_fsearch:
            self.app_dir = self.home / "Library/Application Support/FSearch"
            self.app_dir.mkdir(parents=True, exist_ok=True)
            master_dir = real_home / "Library/Application Support/FSearch"
            if master_dir.is_dir():
                for item in master_dir.iterdir():
                    if item.name.endswith(".sock") or item.name.endswith(".lock"):
                        continue
                    try:
                        os.symlink(item, self.app_dir / item.name)
                    except OSError:
                        pass
            cmd = [binary_path, "serve"]
        else:
            self.cache_dir = self.home / "Library/Caches/fplussearch"
            self.cache_dir.mkdir(parents=True, exist_ok=True)
            master_cache = real_home / "Library/Caches/fplussearch"
            if master_cache.is_dir():
                for f in master_cache.glob("index_*"):
                    try:
                        os.symlink(f, self.cache_dir / f.name)
                    except OSError:
                        pass
            cmd = [binary_path, "serve", "--root", root_path, "--no-rescan"]

        self.log_file = open(self.home / "daemon.log", "w+")
        self.proc = subprocess.Popen(
            cmd,
            env={**os.environ, "HOME": str(self.home)},
            stdout=self.log_file,
            stderr=self.log_file
        )
        
        deadline = time.monotonic() + 30
        self.sock = None
        sock_search_dir = self.app_dir if self.is_fsearch else self.cache_dir
        while time.monotonic() < deadline:
            if self.proc.poll() is not None:
                self.log_file.seek(0)
                raise RuntimeError(f"Daemon exited: {self.log_file.read()[-2000:]}")
            socks = list(sock_search_dir.glob("*.sock"))
            if socks:
                try:
                    s = socket.socket(socket.AF_UNIX)
                    s.settimeout(60)
                    s.connect(str(socks[0]))
                    self.sock = s
                    self.f = s.makefile("rw")
                    break
                except (ConnectionRefusedError, FileNotFoundError):
                    pass
            time.sleep(0.05)
            
        if self.sock is None:
            raise RuntimeError("Daemon socket connection timed out")
            
        # Wait for daemon readiness
        deadline = time.monotonic() + 60
        while time.monotonic() < deadline:
            try:
                res = self.call({"op": "status"})
                if res.get("ok"):
                    if not self.is_fsearch:
                        if res.get("ready") and not res.get("busy"):
                            break
                    else:
                        break
            except RuntimeError:
                pass
            time.sleep(0.05)

    def call(self, req: dict):
        self.f.write(json.dumps(req) + "\n")
        self.f.flush()
        line = self.f.readline()
        if not line:
            raise RuntimeError("Connection closed by daemon")
        data = json.loads(line)
        if not data.get("ok"):
            raise RuntimeError(f"Error from daemon: {data}")
        return data

    def query(self, q: str, limit: int = 50):
        t0 = time.perf_counter_ns()
        res = self.call({"q": q, "limit": limit})
        ms = (time.perf_counter_ns() - t0) / 1e6
        h = 0xcbf29ce484222325
        for hit in res.get("hits", []):
            p = hit.get("path", "").encode()
            h = fnv64(p, h)
            if "score" in hit:
                score = hit["score"]
                h = fnv64(score.to_bytes(4, "little", signed=True), h)
            if "line" in hit:
                line = hit["line"]
                h = fnv64(line.to_bytes(4, "little"), h)
                h = fnv64(hit.get("symbol", "").encode(), h)
        return ms, h

    def grep(self, pattern: str, mode: str = "literal", q: str = "", limit: int = 50):
        t0 = time.perf_counter_ns()
        req = {
            "op": "grep",
            "pattern": pattern,
            "mode": mode,
            "limit": limit,
            "per_file": 1,
            "budget_ms": 0
        }
        if q:
            req["q"] = q
        res = self.call(req)
        ms = (time.perf_counter_ns() - t0) / 1e6
        h = 0xcbf29ce484222325
        for f in res.get("files", []):
            h = fnv64(f.get("path", "").encode(), h)
            lines = f.get("lines") or f.get("matches", [])
            for line in lines:
                h = fnv64(line.get("line", 0).to_bytes(4, "little"), h)
                h = fnv64(line.get("text", "").encode(), h)
        return ms, h

    def close(self):
        try:
            self.f.close()
            self.sock.close()
        except Exception:
            pass
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(timeout=2)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        self.log_file.close()
        self.tmp.cleanup()


def main():
    if len(sys.argv) < 4:
        print("usage: python3 demo/rig.py <binary> <corpus.json> <out.json> [kinds]")
        sys.exit(1)

    binary = sys.argv[1]
    corpus_path = sys.argv[2]
    out_path = sys.argv[3]
    kinds_arg = sys.argv[4] if len(sys.argv) > 4 else "name,typing,grep"
    kinds = set(kinds_arg.split(","))

    with open(corpus_path) as f:
        corpus = json.load(f)

    res = {}
    def record(kind, cat, q, ms, digest):
        key = f"{kind} {q}"
        if key not in res:
            res[key] = {
                "kind": kind,
                "cat": cat,
                "ms": [],
                "digest": f"{digest:016x}"
            }
        res[key]["ms"].append(ms)

    client = DaemonClient(binary)
    try:
        # 1. Name queries
        names = corpus.get("names", [])
        if "name" in kinds and names:
            # Warm up
            for item in names[:15]:
                client.query(item["q"])

            for _ in range(5):
                for item in names:
                    cat, q = item["cat"], item["q"]
                    elapsed_ms, digest = client.query(q)
                    record("name", cat, q, elapsed_ms, digest)

        # 2. Typing runs
        typing = corpus.get("typing", [])
        if "typing" in kinds and typing:
            for _ in range(3):
                for run in typing:
                    t0 = time.perf_counter_ns()
                    d = 0
                    for k in run:
                        _, hit_digest = client.query(k)
                        d ^= hit_digest
                    elapsed_ms = (time.perf_counter_ns() - t0) / 1e6
                    record("typing", "typing", run[-1], elapsed_ms, d)

        # 3. Grep queries
        grep = corpus.get("grep", [])
        if "grep" in kinds and grep:
            # Warm up
            for item in grep[:5]:
                raw_q = item["q"]
                if raw_q.startswith("grep:"):
                    client.grep(raw_q[5:], mode="literal")
                elif raw_q.startswith("regex:"):
                    client.grep(raw_q[6:], mode="regex")

            for _ in range(3):
                for item in grep:
                    cat, raw_q = item["cat"], item["q"]
                    if raw_q.startswith("grep:"):
                        elapsed_ms, digest = client.grep(raw_q[5:], mode="literal")
                    elif raw_q.startswith("regex:"):
                        elapsed_ms, digest = client.grep(raw_q[6:], mode="regex")
                    else:
                        elapsed_ms, digest = client.query(raw_q)
                    record("grep", cat, raw_q, elapsed_ms, digest)

        os.makedirs(os.path.dirname(os.path.abspath(out_path)), exist_ok=True)
        with open(out_path, "w") as f:
            json.dump(res, f)

    finally:
        client.close()


if __name__ == "__main__":
    main()
