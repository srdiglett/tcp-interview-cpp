"""Black-box checks of queue admission, worker reuse and signal shutdown."""
import contextlib
import os
import select
import signal
import socket
import struct
import subprocess
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
    def server(self, workers=1, capacity=1):
        with socket.socket() as probe:
            probe.bind(("127.0.0.1", 0))
            port = probe.getsockname()[1]
        process = subprocess.Popen(
            [BINARY, "--port", str(port), "--workers", str(workers),
             "--queue-capacity", str(capacity)],
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


if __name__ == "__main__":
    unittest.main()
