"""Local protocol regressions. Usage: python3 tests/backend.py /path/to/ttyd."""
import base64
import http.client
import json
import os
import queue
import socket
import struct
import subprocess
import sys
import threading
import time
import unittest

BINARY = os.path.abspath(sys.argv.pop(1))


class Closed(Exception):
    pass


class Server:
    def __init__(self, options=(), command=None):
        env = dict(os.environ)
        # PTY children and libwebsockets have independent shutdown lifetimes.
        # Low-level C unit tests separately run LeakSanitizer with leak checking on.
        env.setdefault("ASAN_OPTIONS", "detect_leaks=0:abort_on_error=1")
        env.setdefault("UBSAN_OPTIONS", "halt_on_error=1:print_stacktrace=1")
        self.process = subprocess.Popen(
            [BINARY, "-i", "127.0.0.1", "-p", "0", *options,
             *(command or ["/bin/sh", "-c", "printf READY; exec /bin/sh"])],
            stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL, stderr=subprocess.PIPE,
            env=env, text=True,
        )
        self.logs = []
        ports = queue.Queue()
        def collect():
            for line in self.process.stderr:
                self.logs.append(line)
                if "Listening on port:" in line:
                    ports.put(int(line.rsplit(":", 1)[1]))
        self.reader = threading.Thread(target=collect, daemon=True)
        self.reader.start()
        try:
            self.port = ports.get(timeout=5)
        except queue.Empty:
            self.stop()
            raise AssertionError("Server failed to start: " + "".join(self.logs))

    def stop(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(timeout=5)
                raise AssertionError("Server failed to stop")
        self.reader.join(timeout=2)
        self.process.stderr.close()

    def healthy(self):
        assert self.process.poll() is None, "Daemon died: " + "".join(self.logs)
        assert not any("AddressSanitizer" in line or "runtime error:" in line for line in self.logs), "".join(self.logs)

    def request(self, path="/token", authorization=None):
        conn = http.client.HTTPConnection("127.0.0.1", self.port, timeout=5)
        conn.request("GET", path, headers={"Authorization": authorization} if authorization else {})
        response = conn.getresponse()
        body = response.read()
        status = response.status
        conn.close()
        return status, body

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.stop()
        assert self.process.returncode == 0, "".join(self.logs)
        assert not any("AddressSanitizer" in line or "runtime error:" in line for line in self.logs), "".join(self.logs)


class WebSocket:
    def __init__(self, server, path="/ws", authorization=None):
        self.socket = socket.create_connection(("127.0.0.1", server.port), timeout=5)
        self.pending = bytearray()
        key = base64.b64encode(os.urandom(16)).decode()
        request = (f"GET {path} HTTP/1.1\r\nHost: 127.0.0.1:{server.port}\r\n"
                   "Upgrade: websocket\r\nConnection: Upgrade\r\n"
                   f"Sec-WebSocket-Key: {key}\r\nSec-WebSocket-Version: 13\r\n"
                   "Sec-WebSocket-Protocol: tty\r\n")
        if authorization:
            request += f"Authorization: {authorization}\r\n"
        self.socket.sendall((request + "\r\n").encode())
        header = bytearray()
        while b"\r\n\r\n" not in header:
            chunk = self.socket.recv(4096)
            if not chunk:
                self.socket.close()
                raise Closed()
            header.extend(chunk)
        raw, rest = header.split(b"\r\n\r\n", 1)
        self.status = int(raw.split(b" ")[1])
        self.pending.extend(rest)

    def send(self, payload, final=True, opcode=2):
        payload = payload.encode() if isinstance(payload, str) else payload
        size = len(payload)
        mask = os.urandom(4)
        header = bytes([(128 if final else 0) | opcode])
        header += bytes([128 | size]) if size < 126 else (bytes([128 | 126]) + struct.pack("!H", size) if size <= 65535 else bytes([128 | 127]) + struct.pack("!Q", size))
        self.socket.sendall(header + mask + bytes(value ^ mask[i % 4] for i, value in enumerate(payload)))

    def read(self, count):
        while len(self.pending) < count:
            data = self.socket.recv(max(4096, count - len(self.pending)))
            if not data:
                raise Closed()
            self.pending.extend(data)
        result = bytes(self.pending[:count])
        del self.pending[:count]
        return result

    def recv(self):
        while True:
            first, second = self.read(2)
            size = second & 127
            if size == 126:
                size = struct.unpack("!H", self.read(2))[0]
            elif size == 127:
                size = struct.unpack("!Q", self.read(8))[0]
            data = self.read(size)
            opcode = first & 15
            if opcode == 8:
                raise Closed()
            if opcode == 9:
                self.send(data, opcode=10)
                continue
            return data

    def init(self):
        assert self.status == 101
        self.send(json.dumps({"columns": 80, "rows": 24}))
        title, preferences = self.recv(), self.recv()
        assert title.startswith(b"1") and preferences.startswith(b"2")
        return title, json.loads(preferences[1:])

    def until(self, expected, timeout=5):
        received = bytearray()
        end = time.monotonic() + timeout
        while expected not in received and time.monotonic() < end:
            self.socket.settimeout(max(0.1, end - time.monotonic()))
            packet = self.recv()
            if packet.startswith(b"0"):
                received.extend(packet[1:])
        assert expected in received, received
        return received

    def close(self):
        self.socket.close()

    def __enter__(self):
        return self

    def __exit__(self, *unused):
        self.close()


class BackendTests(unittest.TestCase):
    def test_empty_and_malformed_messages_do_not_kill_daemon(self):
        invalid = [b"", b"{", b'{"columns":0}', b'{"rows":-1}', b'{"columns":65536}', b'{"rows":"24"}', b"0before-init", b"{} trailing", b'{"rows":2.5}']
        with Server() as server:
            for payload in invalid:
                with self.subTest(payload=payload), WebSocket(server) as ws:
                    ws.send(payload)
                    with self.assertRaises((Closed, ConnectionResetError)):
                        ws.recv()
                    server.healthy()
            self.assertEqual(server.request()[0], 200)

    def test_fragmented_init_and_input_resize(self):
        with Server(["-W"]) as server, WebSocket(server) as ws:
            ws.send(b"", final=False)
            ws.send(b'{"columns":80,', final=False, opcode=0)
            ws.send(b'"rows":24}', opcode=0)
            self.assertTrue(ws.recv().startswith(b"1"))
            self.assertTrue(ws.recv().startswith(b"2"))
            ws.until(b"READY")
            ws.send(b'1{"columns":91,"rows":31}')
            ws.send(b"0stty size\n")
            ws.until(b"31 91")
            ws.send(b"0")
            ws.send("0printf '\\nBACKEND_OK_世界\\n'\n")
            ws.until("BACKEND_OK_世界".encode())

    def test_long_initial_messages(self):
        value = "x" * 6000
        with Server(["-t", "titleFixed=" + value]) as server, WebSocket(server) as ws:
            _, prefs = ws.init()
            self.assertEqual(prefs["titleFixed"], value)
            ws.until(b"READY")
        with Server(command=["/bin/sh", "-c", "printf READY; sleep 1", value]) as server, WebSocket(server) as ws:
            title, _ = ws.init()
            self.assertIn(value.encode(), title)

    def test_long_credential_token_and_authorization(self):
        credential = base64.b64encode(("u:" + "x" * 184).encode()).decode()
        with Server(["-c", "u:" + "x" * 184]) as server:
            for _ in range(3):
                status, body = server.request(authorization="Basic " + credential)
                self.assertEqual(status, 200)
                self.assertEqual(json.loads(body)["token"], credential)
            self.assertEqual(server.request()[0], 401)
            self.assertEqual(server.request(authorization="Basic " + "x" * 1000)[0], 401)

    def test_pause_resume_stops_output(self):
        script = "import time;\nwhile True:\n print('TICK',flush=True);time.sleep(.01)"
        with Server(command=[sys.executable, "-u", "-c", script]) as server, WebSocket(server) as ws:
            ws.init()
            ws.until(b"TICK")
            ws.send(b"2")
            ws.socket.settimeout(0.15)
            try:
                while True:
                    ws.recv()
            except socket.timeout:
                pass
            ws.socket.settimeout(0.25)
            with self.assertRaises(socket.timeout):
                ws.recv()
            ws.send(b"3")
            ws.until(b"TICK")

    def test_child_exit_drains_output_after_resume(self):
        script = "import time; print('READY',flush=True); time.sleep(.2); print('FINAL_OUTPUT',flush=True)"
        with Server(command=[sys.executable, "-u", "-c", script]) as server, WebSocket(server) as ws:
            ws.init()
            ws.until(b"READY")
            ws.send(b"2")
            time.sleep(.4)
            ws.send(b"3")
            ws.until(b"FINAL_OUTPUT")

    def test_paths_and_base_path_bounds(self):
        with Server(["-b", "/terminal/"]) as server:
            self.assertEqual(server.request("/terminal/token")[0], 200)
            try:
                with WebSocket(server, "/terminal/w") as ws:
                    self.assertNotEqual(ws.status, 101)
            except Closed:
                pass
            with WebSocket(server, "/terminal/ws") as ws:
                ws.init()
                ws.until(b"READY")
        with Server(["-b", "/bin/sh"]) as server, WebSocket(server, "/bin/sh/ws") as ws:
            ws.init()
            ws.until(b"READY")
        with Server(["-b", "/" + "x" * 120]) as server:
            self.assertEqual(server.request("/" + "x" * 120 + "/token")[0], 200)
        env = dict(os.environ, ASAN_OPTIONS="detect_leaks=0:abort_on_error=1")
        for arguments in [["-b", "/" + "x" * 127], ["-p", "70000"], ["-p", "12junk"], ["-s", "H"], ["-c", "u:" + "x" * 500]]:
            result = subprocess.run([BINARY, *arguments, "/bin/sh"], capture_output=True, env=env, timeout=5)
            self.assertNotEqual(result.returncode, 0)
            self.assertNotIn(b"AddressSanitizer", result.stderr)

    def test_large_message_limit(self):
        with Server() as server, WebSocket(server) as ws:
            try:
                ws.send(b"x" * (16 * 1024 * 1024 + 1))
                with self.assertRaises((Closed, ConnectionResetError)):
                    ws.recv()
            except (BrokenPipeError, ConnectionResetError):
                pass
            server.healthy()
            self.assertEqual(server.request()[0], 200)

    def test_repeated_child_exit_and_disconnect(self):
        with Server(command=["/bin/sh", "-c", "printf DONE"]) as server:
            for _ in range(12):
                with WebSocket(server) as ws:
                    ws.init()
                    # Exit may race the final PTY read; the daemon must remain usable.
                    try:
                        while ws.recv():
                            pass
                    except (Closed, ConnectionResetError):
                        pass
                server.healthy()


if __name__ == "__main__":
    unittest.main(verbosity=2)
