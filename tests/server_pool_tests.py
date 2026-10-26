"""Black-box checks of queue admission, worker reuse and signal shutdown."""
import contextlib
import os
import select
import signal
import socket
import struct
import subprocess
import time
import unittest

BINARY = os.environ.get("SERVER_BIN", "./build/tcp_server")


def line(connection):
    data = b""
    while not data.endswith(b"\n"):
        chunk = connection.recv(1)
        if not chunk:
            raise AssertionError("connection closed before a complete response")
        data += chunk
    return data.rstrip(b"\r\n")


class PoolTests(unittest.TestCase):
    @contextlib.contextmanager
    def server(self, workers=1, capacity=1, timeout_ms=10000, extra_args=()):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        process = subprocess.Popen(
            [BINARY, "--port", str(port), "--workers", str(workers),
             "--queue-capacity", str(capacity), "--io-timeout-ms", str(timeout_ms), *extra_args],
            stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
        connections = []

        def connect():
            connection = socket.create_connection(("127.0.0.1", port), timeout=3)
            connections.append(connection)
            return connection

        try:
            self.assertTrue(select.select([process.stdout], [], [], 5)[0], "startup timeout")
            self.assertIn("listening on", process.stdout.readline())
            yield process, connect
        finally:
            for connection in connections:
                connection.close()
            if process.poll() is None:
                process.terminate()
            try:
                process.communicate(timeout=5)
            except subprocess.TimeoutExpired:
                process.kill()
                process.communicate()
                self.fail("server did not stop")

    def test_bounded_queue_and_worker_reuse(self):
        with self.server() as (_, connect):
            active = connect()
            self.assertIn(b"ready", line(active))
            waiting = connect()
            self.assertFalse(select.select([waiting], [], [], .2)[0])
            rejected = connect()
            try:
                self.assertEqual(rejected.recv(1), b"")
            except ConnectionResetError:
                pass
            active.sendall(b"QUIT\n")
            self.assertEqual(line(active), b"BYE")
            self.assertIn(b"ready", line(waiting))
            waiting.sendall(b"PING\nQUIT\n")
            self.assertEqual(line(waiting), b"PONG")
            self.assertEqual(line(waiting), b"BYE")

    def test_idle_client_does_not_block_another_worker(self):
        with self.server(workers=2) as (_, connect):
            idle = connect()
            line(idle)
            other = connect()
            line(other)
            other.sendall(b"PING\nSTATS\n")
            self.assertEqual(line(other), b"PONG")
            self.assertIn(b"active=2", line(other))

    def test_stop_with_active_and_queued_clients(self):
        for sig in (signal.SIGINT, signal.SIGTERM):
            with self.subTest(signal=sig), self.server() as (process, connect):
                active = connect()
                line(active)
                waiting = connect()
                self.assertFalse(select.select([waiting], [], [], .2)[0])
                process.send_signal(sig)
                self.assertEqual(process.wait(timeout=5), 0)
                for connection in (active, waiting):
                    try:
                        self.assertEqual(connection.recv(1), b"")
                    except ConnectionResetError:
                        pass

    def test_stop_with_workers_waiting_on_condition(self):
        with self.server(workers=4) as (process, _):
            process.terminate()
            self.assertEqual(process.wait(timeout=5), 0)

    def test_abrupt_disconnect_does_not_kill_server(self):
        with self.server() as (process, connect):
            broken = connect()
            line(broken)
            broken.setsockopt(socket.SOL_SOCKET, socket.SO_LINGER, struct.pack("ii", 1, 0))
            broken.sendall(b"PING\n")
            broken.close()
            healthy = connect()
            line(healthy)
            healthy.sendall(b"PING\n")
            self.assertEqual(line(healthy), b"PONG")
            self.assertIsNone(process.poll())

    def test_idle_timeout_releases_worker_to_queued_client(self):
        with self.server(timeout_ms=500) as (_, connect):
            idle = connect()
            line(idle)
            waiting = connect()
            self.assertIn(b"ready", line(waiting))
            self.assertEqual(idle.recv(1), b"")
            waiting.sendall(b"PING\n")
            self.assertEqual(line(waiting), b"PONG")

    def test_partial_bytes_do_not_reset_deadline(self):
        with self.server(timeout_ms=800) as (_, connect):
            slow = connect()
            line(slow)
            started = time.monotonic()
            for byte in (b"P", b"I", b"N"):
                slow.sendall(byte)
                time.sleep(.15)
            slow.settimeout(.6)
            self.assertEqual(slow.recv(1), b"")
            self.assertLess(time.monotonic() - started, 1.1)

    def test_fragmented_and_coalesced_frames(self):
        with self.server() as (_, connect):
            client = connect()
            line(client)
            client.sendall(b"PI")
            self.assertFalse(select.select([client], [], [], .1)[0])
            client.sendall(b"NG\nECHO hello\nQUIT\n")
            self.assertEqual(line(client), b"PONG")
            self.assertEqual(line(client), b"hello")
            self.assertEqual(line(client), b"BYE")

    def test_eof_does_not_execute_incomplete_command(self):
        with self.server() as (_, connect):
            client = connect()
            line(client)
            client.sendall(b"PING")
            client.shutdown(socket.SHUT_WR)
            self.assertEqual(client.recv(1), b"")

    def test_frame_size_boundary(self):
        with self.server() as (_, connect):
            client = connect()
            line(client)
            client.sendall(b"ECHO " + b"a" * 4091 + b"\n")
            self.assertEqual(line(client), b"a" * 4091)
            client.sendall(b"a" * 4097 + b"\n")
            try:
                self.assertEqual(client.recv(1), b"")
            except ConnectionResetError:
                pass

    def test_invalid_numeric_arguments(self):
        for option, value in (("--port", "9000oops"), ("--port", "65536"),
                              ("--io-timeout-ms", "0"), ("--io-timeout-ms", "-1"),
                              ("--io-timeout-ms", "3600001")):
            with self.subTest(option=option, value=value):
                result = subprocess.run([BINARY, option, value], capture_output=True, timeout=3)
                self.assertEqual(result.returncode, 2)

    def test_json_escape_cannot_inject_response_lines(self):
        with self.server() as (_, connect):
            client = connect()
            line(client)
            client.sendall(b'{"command":"ECHO","text":"one\\ntwo"}\nPING\n')
            self.assertEqual(line(client), b"ERR multiline fields are not allowed")
            self.assertEqual(line(client), b"PONG")


if __name__ == "__main__":
    unittest.main()
