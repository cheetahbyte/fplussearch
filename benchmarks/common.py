"""Seeded search workload and JSON-lines client shared by benchmark runners."""

import json
import os
from pathlib import Path
import random
import re
import socket
import statistics
import subprocess
import time


def stats(values):
    values = sorted(values)
    return {"p50": statistics.median(values), "p90": values[int(len(values) * .9)], "mean": statistics.mean(values), "total": sum(values), "n": len(values)}


def workload(root, sort_files=True):
    rng = random.Random(1)
    files = {}
    raw = subprocess.run(["fd", "-t", "f", "-0", ".", str(root)], capture_output=True, check=True).stdout
    paths = raw.decode().split("\0")[:-1]
    for path in sorted(paths) if sort_files else paths:
        files.setdefault(os.path.basename(path), []).append(path)
    pool = sorted(n for n, ps in files.items() if len(ps) == 1 and 6 <= len(n) <= 32 and n.isascii() and " " not in n and sum(c.isalpha() for c in n) >= 5)
    if len(pool) < 300:
        raise RuntimeError("dataset needs at least 300 unique eligible filenames")
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
    if not sources:
        raise RuntimeError("dataset has no eligible source files")
    identifiers = set()
    for _ in range(10000):
        if len(identifiers) == 60:
            break
        words = re.findall(r"\b[a-z][a-z0-9_]{9,30}\b", Path(rng.choice(sources)).read_text(errors="ignore"))
        if words:
            identifiers.add(rng.choice(words))
    if len(identifiers) != 60:
        raise RuntimeError("could not select 60 content patterns")
    return len(paths), queries, sorted(identifiers) + ["mutex_lock", "kmalloc", "EXPORT_SYMBOL_GPL", "spin_lock_irqsave", "struct device", "TODO", "return -EINVAL"]


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
