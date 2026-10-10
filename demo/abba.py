"""Compare two builds of fplussearch, interleaved A B B A so drift on a
busy machine hits both alike. Prints per-category speed and every query
whose results differ.

usage: python3 demo/abba.py <binary A> <binary B> [corpus.json] [rounds] [kinds]
"""

import json
import math
import os
import subprocess
import sys
import tempfile
from collections import defaultdict

a = sys.argv[1] if len(sys.argv) > 1 else "build/fplussearch.orig"
b = sys.argv[2] if len(sys.argv) > 2 else "build/fplussearch"
corpus = sys.argv[3] if len(sys.argv) > 3 and not sys.argv[3].isdigit() else "demo/corpus.json"
rounds_idx = 4 if len(sys.argv) > 3 and not sys.argv[3].isdigit() else 3
rounds = int(sys.argv[rounds_idx]) if len(sys.argv) > rounds_idx and sys.argv[rounds_idx].isdigit() else 1
kinds_idx = rounds_idx + 1
kinds = sys.argv[kinds_idx:kinds_idx + 1]

if not os.path.exists(corpus):
    # Auto-generate corpus if missing
    print(f"Generating benchmark corpus at {corpus}...")
    subprocess.run(["python3", "demo/gen_corpus.py", corpus], check=True)

samples = {"A": defaultdict(list), "B": defaultdict(list)}
meta, digest = {}, {"A": {}, "B": {}}

with tempfile.TemporaryDirectory() as tmp:
    for r in range(rounds):
        for arm in "ABBA":
            out = os.path.join(tmp, f"{arm}{r}.json")
            bin_path = a if arm == "A" else b
            cmd = ["python3", "demo/rig.py", bin_path, corpus, out, *kinds]
            subprocess.run(cmd, check=True, stdout=subprocess.DEVNULL)
            with open(out) as f:
                data = json.load(f)
            for k, v in data.items():
                if not isinstance(v, dict):
                    continue
                samples[arm][k] += v["ms"]
                meta[k] = (v["kind"], v["cat"])
                digest[arm][k] = v["digest"]


def med(x):
    x = sorted(x)
    return x[len(x) // 2]


cats = defaultdict(list)
for k in meta:
    if samples["A"][k] and samples["B"][k]:
        cats[meta[k]].append((med(samples["A"][k]), med(samples["B"][k])))

print(f"{'kind':7} {'cat':8} {'n':>3}  {'A ms':>8} {'B ms':>8}  {'B/A':>6}")
total = []
for (kind, cat), pairs in sorted(cats.items()):
    ga = math.exp(sum(math.log(max(p[0], 1e-3)) for p in pairs) / len(pairs))
    gb = math.exp(sum(math.log(max(p[1], 1e-3)) for p in pairs) / len(pairs))
    total += [math.log(max(p[1], 1e-3) / max(p[0], 1e-3)) for p in pairs]
    print(f"{kind:7} {cat:8} {len(pairs):3}  {ga:8.3f} {gb:8.3f}  {gb / ga:6.3f}")

if total:
    print(f"overall geomean B/A {math.exp(sum(total) / len(total)):.3f}")
diff = [k for k in meta if digest["A"].get(k) != digest["B"].get(k)]
print(f"{len(diff)} queries with different results" + "".join(f"\n  {k}" for k in diff[:30]))
