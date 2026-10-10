"""Convert an xctrace time-profile XML export into weighted folded stacks."""

import argparse
from collections import Counter
import json
from pathlib import Path
import re
import xml.etree.ElementTree as ET


def label(name):
    name = name.replace("std::__1::", "std::")
    name = re.sub(r"\[abi:[^]]+\]", "", name)
    if "Scan::candidates<" in name:
        return "Scan::candidates"
    if "Engine::search(" in name and "operator()" in name:
        return "Engine::search job"
    if "__thread_proxy<" in name:
        return "std::__thread_proxy"
    if "__invoke" in name or "__function::__func<" in name:
        return "std::invoke"
    if "<" in name:
        name = re.sub(r"<.*>", "<...>", name)
    return name.replace(";", ":").replace("\n", " ")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("xml", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--start", type=float, default=0)
    parser.add_argument("--end", type=float, default=float("inf"))
    args = parser.parse_args()
    root = ET.parse(args.xml).getroot()
    ids = {element.attrib["id"]: element for element in root.iter() if "id" in element.attrib}

    def resolve(element):
        return ids[element.attrib["ref"]] if "ref" in element.attrib else element

    stacks, query_stacks, self_time, inclusive, categories, locations = (Counter() for _ in range(6))
    rows = missing = 0
    for row in root.iter("row"):
        elements = list(row)
        if len(elements) != 7:
            continue
        timestamp = int(resolve(elements[0]).text) / 1e9
        if not args.start <= timestamp <= args.end:
            continue
        state = resolve(elements[4]).text
        if state != "Running":
            continue
        weight = int(resolve(elements[5]).text)
        trace = resolve(elements[6])
        if trace.tag != "tagged-backtrace":
            missing += 1
            continue
        frames = [resolve(frame).attrib.get("name", "unknown") for frame in trace]
        if not frames:
            missing += 1
            continue
        names = [label(frame) for frame in reversed(frames)]
        names = [name for i, name in enumerate(names) if i == 0 or name != names[i - 1]]
        thread = resolve(elements[1]).attrib.get("fmt", "thread")
        role = "main" if "Main Thread" in thread else "worker"
        folded = ";".join([role] + names)
        stacks[folded] += weight
        rows += 1
        self_time[names[-1]] += weight
        source = resolve(trace[0]).find("source")
        if source is not None and source.find("path") is not None:
            path = resolve(source.find("path")).text
            locations[f"{Path(path).name}:{source.attrib.get('line', '?')} {names[-1]}"] += weight
        for name in set(names):
            inclusive[name] += weight
        is_query = any("Engine::search" in frame or "ContentIndex::grep" in frame for frame in frames)
        if is_query:
            query_stacks[folded] += weight
        if any("yield" in frame or frame == "swtch_pri" for frame in frames):
            categories["yield"] += weight
        elif any("Pool::loop" in frame for frame in frames) and not is_query:
            categories["worker housekeeping"] += weight
        elif is_query:
            categories["query work"] += weight
        else:
            categories["other"] += weight
    args.output.mkdir(parents=True, exist_ok=True)
    for filename, values in [("all.folded", stacks), ("query.folded", query_stacks)]:
        (args.output / filename).write_text("".join(f"{stack} {weight}\n" for stack, weight in sorted(values.items())))
    total = sum(stacks.values())
    report = {
        "rows": rows,
        "missing_stacks": missing,
        "sampled_cpu_seconds": total / 1e9,
        "categories_percent": {name: 100 * value / total for name, value in categories.items()},
        "self_percent": [(name, 100 * value / total) for name, value in self_time.most_common(25)],
        "inclusive_percent": [(name, 100 * value / total) for name, value in inclusive.most_common(40)],
        "self_locations_percent": [(name, 100 * value / total) for name, value in locations.most_common(30)],
    }
    (args.output / "summary.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
