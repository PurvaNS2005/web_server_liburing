#!/usr/bin/env python3
"""Real-socket regression tests. Uses isolated fixtures and ephemeral loopback ports."""
import argparse
import concurrent.futures
import contextlib
import hashlib
import http.client
import json
import os
from pathlib import Path
import select
import signal
import socket
import struct
import subprocess
import tempfile
import time
import unittest

ARGS = None


class RunningServer:
    def __init__(self, root, *options):
        self.logs = tempfile.TemporaryDirectory(prefix="zerohttpd-test-logs-")
        self.metrics = Path(self.logs.name) / "metrics.jsonl"
        self.error_path = Path(self.logs.name) / "stderr.log"
        self.error = self.error_path.open("w+")
        self.process = subprocess.Popen(
            [ARGS.server, "--root", str(root), "--port", "0", "--metrics-file",
             str(self.metrics), "--metrics-interval-ms", "50", *map(str, options)],
            stdout=subprocess.PIPE, stderr=self.error, text=True)
        deadline = time.monotonic() + 8
        line = ""
        while time.monotonic() < deadline:
            if select.select([self.process.stdout], [], [], .1)[0]:
                line = self.process.stdout.readline()
                if "listening on" in line:
                    self.port = int(line.strip().rsplit(":", 1)[1])
                    return
            if self.process.poll() is not None:
                break
        self.stop(check=False)
        error = self.error_path.read_text()
        self.cleanup()
        raise RuntimeError("server startup failed: " + error)

    def socket(self):
        return socket.create_connection(("127.0.0.1", self.port), timeout=5)

    def fetch(self, path="/", method="GET", headers=None):
        connection = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        try:
            connection.request(method, path, headers=headers or {})
            response = connection.getresponse()
            return response.status, dict(response.getheaders()), response.read()
        finally:
            connection.close()

    def snapshot(self):
        time.sleep(.08)
        lines = self.metrics.read_text().splitlines()
        return json.loads(lines[-1])

    def stop(self, check=True):
        if self.process.poll() is None:
            self.process.send_signal(signal.SIGTERM)
        try:
            self.process.wait(timeout=8)
        except subprocess.TimeoutExpired:
            self.process.kill()
            self.process.wait()
            raise AssertionError("server did not shut down within deadline")
        if self.process.stdout:
            self.process.stdout.close()
        self.error.flush()
        if check and self.process.returncode != 0:
            raise AssertionError(self.error_path.read_text())
        if check:
            text = self.error_path.read_text()
            if "AddressSanitizer" in text or "runtime error:" in text:
                raise AssertionError(text)
        return json.loads(self.metrics.read_text().splitlines()[-1]) if self.metrics.exists() else {}

    def cleanup(self):
        self.error.close()
        self.logs.cleanup()


@contextlib.contextmanager
def server(root, *options):
    running = RunningServer(root, *options)
    try:
        yield running
    finally:
        try:
            running.stop()
        finally:
            running.cleanup()


def wire(path="/", method="GET", extra="", close=True):
    return (f"{method} {path} HTTP/1.1\r\nHost: localhost\r\n" +
            ("Connection: close\r\n" if close else "") + extra + "\r\n").encode()


def response(stream, head=False):
    line = stream.readline()
    if not line:
        raise AssertionError("connection closed before response")
    status = int(line.split()[1])
    headers = {}
    while True:
        line = stream.readline()
        if line == b"\r\n":
            break
        if not line:
            raise AssertionError("truncated header")
        key, value = line.decode().split(":", 1)
        headers[key.lower()] = value.strip()
    size = int(headers["content-length"])
    body = b"" if head else stream.read(size)
    if not head and len(body) != size:
        raise AssertionError(f"truncated body: {len(body)} != {size}")
    return status, headers, body


class Integration(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temp = tempfile.TemporaryDirectory(prefix="zerohttpd-fixtures-")
        cls.root = Path(cls.temp.name) / "public"
        cls.root.mkdir()
        cls.index = b"<html><body>ZeroHTTPd test</body></html>\n"
        (cls.root / "index.html").write_bytes(cls.index)
        (cls.root / "empty.txt").write_bytes(b"")
        (cls.root / "unknown.bin").write_bytes(bytes(range(256)))
        (cls.root / "IMAGE.PNG").write_bytes(b"\x89PNG\x00test")
        (cls.root / "space name.txt").write_bytes(b"encoded name")
        (cls.root / "nested").mkdir()
        (cls.root / "nested" / "index.html").write_bytes(b"nested")
        cls.large = bytes(range(256)) * 32768  # 8 MiB, far larger than one file buffer.
        (cls.root / "large.bin").write_bytes(cls.large)
        (Path(cls.temp.name) / "secret.txt").write_bytes(b"PRIVATE-OUTSIDE-ROOT")
        (cls.root / "escape.txt").symlink_to("../secret.txt")
        (cls.root / "inside.txt").symlink_to("index.html")
        os.mkfifo(cls.root / "fifo.txt")
        cls.running = RunningServer(cls.root)

    @classmethod
    def tearDownClass(cls):
        try:
            metrics = cls.running.stop()
            if metrics["active_connections"] != 0:
                raise AssertionError("connections remained at shutdown")
        finally:
            cls.running.cleanup()
            cls.temp.cleanup()

    def test_static_files(self):
        for path, data, mime in [("/", self.index, "text/html"),
                ("/index.html?version=2", self.index, "text/html"),
                ("/nested/", b"nested", "text/html"),
                ("/empty.txt", b"", "text/plain"),
                ("/unknown.bin", bytes(range(256)), "application/octet-stream"),
                ("/IMAGE.PNG", b"\x89PNG\x00test", "image/png"),
                ("/space%20name.txt", b"encoded name", "text/plain")]:
            with self.subTest(path=path):
                status, headers, body = self.running.fetch(path)
                self.assertEqual(status, 200)
                self.assertEqual(body, data)
                self.assertEqual(int(headers["Content-Length"]), len(data))
                self.assertTrue(headers["Content-Type"].startswith(mime))
                self.assertIn("Date", headers)

    def test_head(self):
        for path, size, status in [("/", len(self.index), 200), ("/large.bin", len(self.large), 200),
                                   ("/missing", len(b"404 Not Found\n"), 404)]:
            result, headers, body = self.running.fetch(path, method="HEAD")
            self.assertEqual(result, status)
            self.assertEqual(int(headers["Content-Length"]), size)
            self.assertEqual(body, b"")

    def test_large_file_integrity(self):
        status, _, body = self.running.fetch("/large.bin")
        self.assertEqual(status, 200)
        self.assertEqual(hashlib.sha256(body).digest(), hashlib.sha256(self.large).digest())

    def test_file_shrinks_mid_transfer(self):
        path = self.root / "shrinking.bin"
        path.write_bytes(self.large)
        with self.running.socket() as sock, sock.makefile("rb") as stream:
            sock.sendall(wire("/shrinking.bin"))
            # Consume only the header while the server encounters socket backpressure.
            while stream.readline() != b"\r\n":
                pass
            path.write_bytes(b"")
            body = stream.read()
            self.assertLess(len(body), len(self.large))
        path.unlink()
        self.assertEqual(self.running.fetch()[0], 200)

    def test_missing_and_nonregular(self):
        for path in ["/absent", "/nested", "/fifo.txt"]:
            self.assertEqual(self.running.fetch(path)[0], 404)
        self.assertEqual(self.running.fetch("/")[0], 200)

    def test_traversal_and_symlinks(self):
        for path in ["/../secret.txt", "/%2e%2e/secret.txt", "/x/%2e%2e/%2e%2e/secret.txt",
                     "/escape.txt", "/%2f..%2fsecret.txt", "/%00index.html", "/%xx"]:
            status, _, body = self.running.fetch(path)
            self.assertIn(status, (400, 403, 404))
            self.assertNotIn(b"PRIVATE-OUTSIDE-ROOT", body)
        self.assertEqual(self.running.fetch("/inside.txt")[2], self.index)

    def test_keep_alive_and_pipeline(self):
        with self.running.socket() as sock, sock.makefile("rb") as stream:
            sock.sendall(wire(close=False))
            self.assertEqual(response(stream)[2], self.index)
            sock.sendall(wire("/empty.txt", close=False) + wire("/unknown.bin"))
            self.assertEqual(response(stream)[2], b"")
            self.assertEqual(response(stream)[2], bytes(range(256)))
            self.assertEqual(stream.read(1), b"")

    def test_fragmented_request(self):
        with self.running.socket() as sock, sock.makefile("rb") as stream:
            for part in [b"G", b"ET / HT", b"TP/1.1\r", b"\nHo", b"st: x\r\nConnection: close\r\n", b"\r\n"]:
                sock.sendall(part)
                time.sleep(.01)
            self.assertEqual(response(stream)[2], self.index)

    def test_http10_close(self):
        with self.running.socket() as sock, sock.makefile("rb") as stream:
            sock.sendall(b"GET / HTTP/1.0\r\n\r\n")
            status, headers, body = response(stream)
            self.assertEqual(status, 200)
            self.assertEqual(headers["connection"], "close")
            self.assertEqual(body, self.index)
            self.assertEqual(stream.read(1), b"")

    def test_invalid_requests_and_bodies(self):
        cases = [(b"\r\n\r\n", 400), (b"GET / HTTP/1.1\r\n\r\n", 400),
                 (wire(method="POST"), 405), (wire(method="get"), 405),
                 (wire(extra="Host: duplicate\r\n"), 400),
                 (wire(extra="Content-Length: 10\r\n") + b"1234567890", 400),
                 (wire(extra="Content-Length: 0\r\nContent-Length: 0\r\n"), 400),
                 (wire(extra="Transfer-Encoding: chunked\r\n") + b"0\r\n\r\n", 400),
                 (wire(extra="Expect: 100-continue\r\n"), 417),
                 (b"GET / HTTP/2.0\r\nHost: x\r\n\r\n", 505)]
        for request, expected in cases:
            with self.subTest(request=request):
                with self.running.socket() as sock, sock.makefile("rb") as stream:
                    sock.sendall(request)
                    status, headers, _ = response(stream)
                    self.assertEqual(status, expected)
                    self.assertEqual(headers["connection"], "close")
        self.assertEqual(self.running.fetch()[0], 200)

    def test_oversized_headers(self):
        with self.running.socket() as sock, sock.makefile("rb") as stream:
            sock.sendall(b"GET / HTTP/1.1\r\nHost: x\r\nX-Pad: " + b"x" * 17000)
            self.assertEqual(response(stream)[0], 431)
        self.assertEqual(self.running.fetch()[0], 200)

    def test_concurrent_transfers(self):
        def fetch(i):
            path = "/large.bin" if i % 4 == 0 else "/"
            status, _, body = self.running.fetch(path)
            return status == 200 and body == (self.large if path == "/large.bin" else self.index)
        with concurrent.futures.ThreadPoolExecutor(max_workers=24) as pool:
            self.assertTrue(all(pool.map(fetch, range(96))))

    def test_disconnect_cleanup(self):
        before = len(list(Path(f"/proc/{self.running.process.pid}/fd").iterdir()))
        for i in range(150):
            with self.running.socket() as sock:
                if i % 3:
                    sock.sendall(wire("/large.bin", close=False))
                    sock.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
        time.sleep(.3)
        self.assertEqual(self.running.fetch()[0], 200)
        after = len(list(Path(f"/proc/{self.running.process.pid}/fd").iterdir()))
        self.assertLessEqual(after, before + 3)

    def test_cache_invalidation(self):
        file = self.root / "mutable.txt"
        file.write_bytes(b"before")
        self.assertEqual(self.running.fetch("/mutable.txt")[2], b"before")
        self.assertEqual(self.running.fetch("/mutable.txt")[2], b"before")
        file.write_bytes(b"AFTER!")  # same-size modification
        self.assertEqual(self.running.fetch("/mutable.txt")[2], b"AFTER!")
        replacement = self.root / "replacement.txt"
        replacement.write_bytes(b"new inode")
        replacement.replace(file)
        self.assertEqual(self.running.fetch("/mutable.txt")[2], b"new inode")
        file.unlink()
        self.assertEqual(self.running.fetch("/mutable.txt")[0], 404)

    def test_metrics(self):
        for _ in range(3):
            self.running.fetch()
        metrics = self.running.snapshot()
        self.assertGreater(metrics["completed"], 0)
        self.assertGreater(metrics["cache_hits"], 0)
        self.assertGreater(metrics["file_reads"], 0)
        self.assertLessEqual(metrics["cache_bytes"], 16 * 1024 * 1024)
        self.assertGreaterEqual(metrics["p99_ms"], metrics["p50_ms"])

    def test_client(self):
        if not ARGS.client:
            self.skipTest("no client binary supplied")
        for path, expected in [("/", 0), ("/large.bin", 0), ("/missing", 1)]:
            proc = subprocess.run([ARGS.client, "127.0.0.1", str(self.running.port), path],
                                  capture_output=True, timeout=10)
            self.assertEqual(proc.returncode, expected, proc.stderr.decode())
            self.assertIn(b"HTTP/1.1", proc.stdout)

    def test_timeout_and_slow_headers(self):
        with server(self.root, "--timeout-ms", 150, "--header-timeout-ms", 250) as running:
            with running.socket() as idle:
                time.sleep(.25)
                self.assertEqual(idle.recv(1), b"")
            with running.socket() as slow:
                slow.sendall(b"GET / HTTP/1.1\r\nX: ")
                for _ in range(3):
                    time.sleep(.07)
                    slow.sendall(b"a")
                time.sleep(.12)
                self.assertEqual(slow.recv(1), b"")
            self.assertEqual(running.fetch()[0], 200)
            self.assertGreaterEqual(running.snapshot()["timeouts"], 2)

    def test_request_limit(self):
        with server(self.root, "--max-requests", 2) as running:
            with running.socket() as sock, sock.makefile("rb") as stream:
                sock.sendall(wire(close=False) * 3)
                self.assertEqual(response(stream)[1]["connection"], "keep-alive")
                self.assertEqual(response(stream)[1]["connection"], "close")
                self.assertEqual(stream.read(1), b"")

    def test_connection_limit(self):
        with server(self.root, "--max-connections", 2, "--timeout-ms", 1000) as running:
            with running.socket() as first, running.socket() as second:
                first.sendall(b"GET / HTTP/1.1\r\n")
                second.sendall(b"GET / HTTP/1.1\r\n")
                self.assertEqual(running.snapshot()["active_connections"], 2)
                first.close()
                self.assertEqual(running.fetch()[0], 200)

    def test_slow_reader_and_shutdown_drain(self):
        with server(self.root, "--cache-bytes", 0, "--buffer-size", 4096) as running:
            with running.socket() as sock:
                sock.sendall(wire("/large.bin"))
                with sock.makefile("rb") as stream:
                    time.sleep(.1)
                    running.process.send_signal(signal.SIGTERM)
                    self.assertEqual(response(stream)[2], self.large)
            running.process.wait(timeout=5)
            self.assertEqual(running.process.returncode, 0)

    def test_cached_body_and_backpressure(self):
        with server(self.root, "--cache-bytes", 16777216, "--cache-file-bytes", 8388608,
                    "--buffer-size", 1048576) as running:
            self.assertEqual(running.fetch("/large.bin")[2], self.large)
            with running.socket() as sock, sock.makefile("rb") as stream:
                sock.sendall(wire("/large.bin"))
                time.sleep(.15)
                self.assertEqual(response(stream)[2], self.large)
            self.assertGreaterEqual(running.snapshot()["cache_hits"], 1)

    def test_sigint(self):
        with server(self.root) as running:
            running.process.send_signal(signal.SIGINT)
            running.process.wait(timeout=5)
            self.assertEqual(running.process.returncode, 0)

    def test_shutdown_deadline(self):
        with server(self.root, "--grace-ms", 100) as running:
            with running.socket() as sock:
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024)
                sock.sendall(wire("/large.bin"))
                time.sleep(.05)
                start = time.monotonic()
                running.stop()
                self.assertLess(time.monotonic() - start, 2)

    def test_tiny_ring_and_cache_budget(self):
        with server(self.root, "--queue-depth", 8, "--batch-size", 8,
                    "--cache-bytes", 64, "--cache-file-bytes", 64, "--buffer-size", 1024) as running:
            with concurrent.futures.ThreadPoolExecutor(max_workers=20) as pool:
                statuses = list(pool.map(lambda _: running.fetch()[0], range(100)))
            self.assertEqual(statuses, [200] * 100)
            self.assertEqual(running.fetch("/large.bin")[2], self.large)
            self.assertLessEqual(running.snapshot()["cache_bytes"], 64)

    def test_cli_validation(self):
        for args in [["--queue-depth", "0"], ["--batch-size", "257"], ["--port", "99999"],
                     ["--root", "/definitely-absent"], ["--unknown", "1"]]:
            proc = subprocess.run([ARGS.server, *args], capture_output=True, timeout=5)
            self.assertNotEqual(proc.returncode, 0)
        proc = subprocess.run([ARGS.server, "--help"], capture_output=True, timeout=5)
        self.assertEqual(proc.returncode, 0)


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--server", required=True)
    parser.add_argument("--client")
    ARGS, rest = parser.parse_known_args()
    ARGS.server = str(Path(ARGS.server).resolve())
    if ARGS.client:
        ARGS.client = str(Path(ARGS.client).resolve())
    unittest.main(argv=[__file__, *rest], verbosity=2)
