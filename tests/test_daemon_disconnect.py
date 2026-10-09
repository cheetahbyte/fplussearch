import json
import os
from pathlib import Path
import socket
import subprocess
import tempfile
import time
import unittest


BINARY = Path(__file__).resolve().parents[1] / "build" / "fplussearch"


def response(connection, request):
    connection.sendall(json.dumps(request).encode() + b"\n")
    data = bytearray()
    while not data.endswith(b"\n"):
        chunk = connection.recv(65536)
        if not chunk:
            raise RuntimeError("daemon closed its socket")
        data.extend(chunk)
    return json.loads(data)


class DaemonDisconnectTest(unittest.TestCase):
    def test_disconnect_during_large_response_preserves_daemon(self):
        with tempfile.TemporaryDirectory(prefix="fplussearch-", dir="/tmp") as directory:
            home = Path(directory)
            root = home / "files"
            root.mkdir()
            cache = home / "Library" / "Caches" / "fplussearch"
            cache.mkdir(parents=True)
            for index in range(2000):
                (root / (f"needle-{index:04}-" + "x" * 180 + ".txt")).touch()
            with (home / "daemon.log").open("w+") as log:
                daemon = subprocess.Popen(
                    [str(BINARY), "serve", "--root", str(root), "--threads", "2"],
                    env={**os.environ, "HOME": str(home)},
                    stdout=log,
                    stderr=log,
                )
                try:
                    deadline = time.monotonic() + 20
                    socket_path = None
                    while time.monotonic() < deadline:
                        self.assertIsNone(daemon.poll(), "daemon exited during startup")
                        paths = list(cache.glob("*.sock"))
                        if paths:
                            socket_path = str(paths[0])
                            with socket.socket(socket.AF_UNIX) as connection:
                                connection.settimeout(5)
                                connection.connect(socket_path)
                                if response(connection, {"op": "status"})["ready"]:
                                    break
                        time.sleep(0.05)
                    else:
                        self.fail("daemon did not become ready")
                    for _ in range(5):
                        with socket.socket(socket.AF_UNIX) as connection:
                            connection.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                            connection.connect(socket_path)
                            connection.sendall(b'{"q":"needle","limit":2000}\n')
                            time.sleep(0.02)
                        time.sleep(0.1)
                        self.assertIsNone(daemon.poll(), "disconnect killed the daemon")
                    with socket.socket(socket.AF_UNIX) as connection:
                        connection.settimeout(5)
                        connection.connect(socket_path)
                        result = response(connection, {"q": "needle", "limit": 1})
                        self.assertTrue(result["ok"])
                        self.assertEqual(result["total"], 2000)
                finally:
                    if daemon.poll() is None:
                        daemon.terminate()
                    try:
                        daemon.wait(timeout=5)
                    except subprocess.TimeoutExpired:
                        daemon.kill()
                        daemon.wait(timeout=5)


if __name__ == "__main__":
    unittest.main()
