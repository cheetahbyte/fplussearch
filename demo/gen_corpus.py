"""Generate a representative benchmark corpus of queries for fplussearch.
Mirrors the structure of fsearch's corpus:
- names: exact, typo, word, multi, filter
- typing: progressive keystroke sequences
- grep: rare, common, regex, sym, scoped
"""

import json
import os
import random
import re
import sys
from pathlib import Path


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else "demo/corpus.json"
    rng = random.Random(0x9E3779B97F4A7C15)

    names = []
    def push_name(cat, q):
        names.append({"cat": cat, "q": q})

    # High quality targets from common developer files & system names
    sample_targets = [
        "CMakeLists.txt", "package.json", "Makefile", "README.md", "Cargo.toml",
        "index.html", "main.cpp", "search.cpp", "content.cpp", "server.cpp",
        "tsconfig.json", "launch.json", "settings.json", "docker-compose.yml",
        "Dockerfile", "gitignore", "format.py", "benchmark.py", "kernel.c",
        "main.rs", "lib.rs", "query.rs", "engine.rs", "memory.cpp",
        "filesystem.hpp", "terminal.cpp", "parser.cpp", "buffer.hpp",
        "symbol.hpp", "overlay.hpp"
    ]

    for t in sample_targets:
        push_name("exact", t)

    # 1-edit typo queries
    for i, t in enumerate(sample_targets):
        chars = list(t)
        letters = [idx for idx, c in enumerate(chars) if c.isalpha()]
        if not letters:
            continue
        p = rng.choice(letters)
        if i % 4 == 0 and p + 1 < len(chars) and chars[p + 1].isalpha() and chars[p] != chars[p + 1]:
            chars[p], chars[p + 1] = chars[p + 1], chars[p]
        elif i % 4 == 1:
            chars.pop(p)
        elif i % 4 == 2:
            chars.insert(p, chr(ord('a') + rng.randint(0, 25)))
        else:
            chars[p] = 'q' if chars[p].lower() == 'x' else 'x'
        push_name("typo", "".join(chars))

    # Common words
    common_words = [
        "main", "test", "search", "config", "util", "helper", "client", "server",
        "data", "file", "node", "index", "engine", "buffer", "stream", "parser",
        "regex", "string", "thread", "vector", "state", "window", "event", "task"
    ]
    for w in common_words:
        push_name("word", w)

    # Multi-token queries (folder / prefix + name)
    multi_queries = [
        "src search", "benchmarks compare", "tests test", "build fplussearch",
        "Developer fplussearch", "include string", "linux kernel", "sys stat",
        "net socket", "doc readme"
    ]
    for m in multi_queries:
        push_name("multi", m)

    # Filters
    filters = [
        "ext:cpp search", "ext:hpp engine", "ext:json config", "type:image png",
        "type:code main", "kind:file size:>100kb", "size:>10mb", "mtime:<7d",
        "'main.cpp", "^build", "a", "e s", "ext:md readme", "!test main"
    ]
    for f in filters:
        push_name("filter", f)

    # Typing runs
    typing_seeds = ["fplussearch", "benchmark", "search.cpp", "config.json", "README.md"]
    typing = []
    for s in typing_seeds:
        typing.append([s[:k] for k in range(1, len(s) + 1)])

    # Grep queries
    grep = [
        {"cat": "common", "q": "grep:include"},
        {"cat": "common", "q": "grep:return"},
        {"cat": "common", "q": "grep:class"},
        {"cat": "common", "q": "grep:struct"},
        {"cat": "common", "q": "grep:static"},
        {"cat": "common", "q": "grep:constexpr"},
        {"cat": "common", "q": "grep:TODO"},
        {"cat": "regex", "q": r"regex:void\s+[a-zA-Z0-9_]+\("},
        {"cat": "regex", "q": r"regex:int[0-9]*_t"},
        {"cat": "regex", "q": r"regex:[A-Z]{4,}_[A-Z]{4,}"},
        {"cat": "sym", "q": "sym:search"},
        {"cat": "sym", "q": "sym:main"},
        {"cat": "sym", "q": "sym:parse_query"},
        {"cat": "scoped", "q": "ext:cpp grep:constexpr"},
        {"cat": "scoped", "q": "ext:hpp grep:struct"},
        {"cat": "scoped", "q": "in:~/Developer grep:fplussearch"},
        {"cat": "rare", "q": "grep:vshrn_n_u16"},
        {"cat": "rare", "q": "grep:kBonusBoundary"},
        {"cat": "rare", "q": "grep:unpack_hex"},
    ]

    corpus = {
        "names": names,
        "typing": typing,
        "grep": grep
    }

    os.makedirs(os.path.dirname(os.path.abspath(out)), exist_ok=True)
    with open(out, "w") as f:
        json.dump(corpus, f, indent=2)
    print(f"Generated corpus with {len(names)} names, {len(typing)} typing runs, and {len(grep)} grep queries at {out}")


if __name__ == "__main__":
    main()
