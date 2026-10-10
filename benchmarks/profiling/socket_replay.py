"""Record isolated filename and content daemon workloads with xctrace."""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from common import Client


def record(client, daemon, requests, output, name):
    for request in requests[:20]:
        client.call(request)
    with (output / f"{name}-recorder.log").open("w") as log:
        recorder = subprocess.Popen([
            "xctrace", "record", "--template", "Time Profiler", "--attach", str(daemon.pid),
            "--time-limit", "12s", "--output", str(output / f"{name}.trace"), "--no-prompt",
        ], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        try:
            for line in recorder.stdout:
                log.write(line)
                log.flush()
                if "Ctrl-C" in line:
                    break
            if recorder.poll() is not None:
                raise RuntimeError(f"profiler exited {recorder.returncode}")
            deadline = time.monotonic() + 14
            completed = 0
            while time.monotonic() < deadline:
                client.call(requests[completed % len(requests)])
                completed += 1
            recorder.wait(timeout=30)
            log.write(recorder.stdout.read())
            if recorder.returncode:
                raise RuntimeError(f"profiler exited {recorder.returncode}")
            log.write(f"\nReplayed {completed} requests for {name}\n")
        finally:
            if recorder.poll() is None:
                recorder.terminate()
                recorder.wait(timeout=30)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("binary", type=Path)
    parser.add_argument("workload", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    data = json.loads(args.workload.read_text())
    root = Path(data["root"])
    args.output.mkdir(parents=True, exist_ok=True)
    scope = f"in:{json.dumps(str(root))}"
    names = [{"q": f"{scope} {query}", "limit": 50} for query, _ in data["workload"]["queries"]]
    content = [{"op": "grep", "q": scope, "pattern": pattern, "mode": "literal", "limit": 50,
                "per_file": 1, "budget_ms": 0} for pattern in data["workload"]["patterns"]]
    if not names or not content:
        raise RuntimeError("workload needs filename queries and content patterns")
    with tempfile.TemporaryDirectory(prefix="fp-profile-", dir="/tmp") as directory:
        home = Path(directory)
        cache = home / "Library/Caches/fplussearch"
        cache.mkdir(parents=True)
        with (args.output / "daemon.log").open("w+") as log:
            daemon = subprocess.Popen([str(args.binary.resolve()), "serve", "--root", str(root)],
                                      env={**os.environ, "HOME": str(home)}, stdout=log, stderr=log)
            client = None
            try:
                deadline = time.monotonic() + 1200
                while time.monotonic() < deadline:
                    if daemon.poll() is not None:
                        raise RuntimeError(f"daemon exited {daemon.returncode}")
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
                    raise RuntimeError("daemon readiness timed out")
                (args.output / "status.json").write_text(json.dumps(status, indent=2))
                record(client, daemon, names, args.output, "filename")
                record(client, daemon, content, args.output, "content")
            finally:
                if client is not None:
                    client.close()
                if daemon.poll() is None:
                    daemon.terminate()
                    try:
                        daemon.wait(timeout=10)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait()


if __name__ == "__main__":
    main()
