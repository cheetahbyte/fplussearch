"""Create a classified engine workload from a saved standalone benchmark."""

import argparse
import json
from pathlib import Path
import random


SHORT = ["a", "c", "h", "x", "ab", "cc", "io", "net", "lib", "src", "foo", "dev",
         "test", "init", "util", "zz", "1", ".", "_", "z"]
NEGATIVE = [
    "!src", "!test", "!a", "!zzzzzzzzz", "!never_present_dir_71", "!third_party",
    "!node_modules", "!drivers", "!include", "!lib", "!linux", "!chromium", "!System",
    "!Library", "!Users", "!Applications", "!foo", "!out", "!build", "!cache",
    "!a !b", "!test !src", "!a !test !lib", "!^test", "!test$", "!'test", "!source",
    "!config", "!README", "!package", "!vendor", "!b", "!c", "!d", "!abc",
    "!zero !one !two !three !four !five !six !seven !eight !nine !ten !eleven !twelve "
    "!thirteen !fourteen !fifteen !sixteen !src",
    "type:file !test", "ext:cpp !third_party", "ext:h !include", "type:dir !test",
]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("saved", type=Path, help="standalone benchmark JSON with a replayable workload")
    parser.add_argument("output", type=Path, help="output TSV: class, tab, query")
    args = parser.parse_args()
    saved = json.loads(args.saved.read_text())
    root = Path(saved["root"])
    if not root.is_dir():
        parser.error("saved dataset folder must exist")
    lines = [("exact" if query == target else "typo", query)
             for query, target in saved["workload"]["queries"]]
    lines += [("short", query) for query in SHORT]
    paths = sorted(path for path in root.rglob("*") if path.is_file())
    rng = random.Random(71)
    for path in rng.sample(paths, min(120, len(paths))):
        ancestors = path.relative_to(root).parts[:-1]
        if not ancestors:
            continue
        directory = rng.choice(ancestors)
        if any(char in directory + path.name for char in ' \t"/:'):
            continue
        name = path.name[:max(2, min(8, len(path.name)))]
        lines += [("multi", directory + " " + name), ("negated", name + " !" + directory)]
    lines += [("negated", query) for query in
              ["!test", "!src", "!a", "foo !test", "a !third_party", "type:file !test"]]
    lines += [("multi", query) for query in
              ["a b c d e", "a b c d e f g h", "src lib test", "foo bar", "a a a a a", "test test"]]
    lines += [("neg_only", query) for query in NEGATIVE]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text("".join(f"{group}\t{query}\n" for group, query in lines))
    print(f"Wrote {len(lines)} queries to {args.output}")


if __name__ == "__main__":
    main()
