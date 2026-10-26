"""Real TLS sockets: trusted clients, rejected peers, deadlines and shutdown."""
import os
from pathlib import Path
import socket
import ssl
import subprocess
import tempfile
import unittest

import server_pool_tests
from server_pool_tests import line, BINARY


class TLSTests(unittest.TestCase):
    server = server_pool_tests.PoolTests.server

    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.pki = Path(cls.temporary.name) / "pki"
        script = Path(__file__).resolve().parents[1] / "scripts/create_test_pki.py"
        subprocess.run([os.sys.executable, str(script), str(cls.pki)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def arguments(self):
        return ["--tls-cert", str(self.pki / "server.crt"),
                "--tls-key", str(self.pki / "server.key"), "--tls-ca", str(self.pki / "ca.crt")]

    def context(self, certificate=True):
        ctx = ssl.create_default_context(cafile=str(self.pki / "ca.crt"))
        if certificate:
            ctx.load_cert_chain(str(self.pki / "client.crt"), str(self.pki / "client.key"))
        return ctx

    def test_trusted_client_and_shutdown(self):
        with self.server(extra_args=self.arguments()) as (process, connect):
            with self.context().wrap_socket(connect(), server_hostname="localhost") as client:
                self.assertIn(b"ready", line(client))
                client.sendall(b"PING\nECHO encrypted\nQUIT\n")
                self.assertEqual(line(client), b"PONG")
                self.assertEqual(line(client), b"encrypted")
                self.assertEqual(line(client), b"BYE")
                self.assertEqual(client.recv(1), b"")
            self.assertIsNone(process.poll())

    def test_missing_certificate(self):
        with self.server(extra_args=self.arguments()) as (_, connect):
            with self.assertRaises((ssl.SSLError, AssertionError, ConnectionResetError)):
                with self.context(False).wrap_socket(connect(), server_hostname="localhost") as client:
                    line(client)

    def test_server_hostname_verified(self):
        with self.server(extra_args=self.arguments()) as (_, connect):
            with self.assertRaises(ssl.SSLCertVerificationError):
                self.context().wrap_socket(connect(), server_hostname="wrong.example")

    def test_untrusted_client(self):
        with tempfile.TemporaryDirectory() as directory:
            other = Path(directory) / "pki"
            subprocess.run([os.sys.executable, str(Path(__file__).resolve().parents[1] /
                           "scripts/create_test_pki.py"), str(other)], check=True)
            ctx = self.context(False)
            ctx.load_cert_chain(str(other / "client.crt"), str(other / "client.key"))
            with self.server(extra_args=self.arguments()) as (_, connect):
                with self.assertRaises((ssl.SSLError, AssertionError, ConnectionResetError)):
                    with ctx.wrap_socket(connect(), server_hostname="localhost") as client:
                        line(client)

    def test_handshake_timeout_releases_worker(self):
        with self.server(timeout_ms=500, extra_args=self.arguments()) as (_, connect):
            silent = connect()
            self.assertEqual(silent.recv(1), b"")
            with self.context().wrap_socket(connect(), server_hostname="localhost") as client:
                self.assertIn(b"ready", line(client))

    def test_signal_during_handshake(self):
        with self.server(extra_args=self.arguments()) as (process, connect):
            connect()
            process.terminate()
            self.assertEqual(process.wait(timeout=3), 0)

    def test_authenticated_idle_timeout(self):
        with self.server(timeout_ms=500, extra_args=self.arguments()) as (_, connect):
            with self.context().wrap_socket(connect(), server_hostname="localhost") as client:
                self.assertIn(b"ready", line(client))
                # Timeout is an abortive close, not necessarily close_notify.
                try:
                    self.assertEqual(client.recv(1), b"")
                except ssl.SSLEOFError:
                    pass

    def test_plaintext_cannot_access_tls_protocol(self):
        with self.server(extra_args=self.arguments()) as (_, connect):
            client = connect()
            client.sendall(b"PING\nQUIT\n")
            try:
                self.assertEqual(client.recv(1), b"")
            except ConnectionResetError:
                pass

    def test_mismatched_private_key_fails_startup(self):
        args = self.arguments()
        args[3] = str(self.pki / "client.key")
        result = subprocess.run([BINARY, *args], capture_output=True, timeout=3)
        self.assertEqual(result.returncode, 1)

    def test_expired_client(self):
        expired = self.pki / "expired.crt"
        (self.pki / "index").write_text("")
        (self.pki / "serial").write_text("1000\n")
        config = self.pki / "ca.conf"
        config.write_text("[ca]\ndefault_ca=lab\n[lab]\n" +
                          f"database={self.pki / 'index'}\nserial={self.pki / 'serial'}\n" +
                          f"new_certs_dir={self.pki}\ncertificate={self.pki / 'ca.crt'}\n" +
                          f"private_key={self.pki / 'ca.key'}\ndefault_md=sha256\n" +
                          "policy=policy\n[policy]\ncommonName=supplied\n")
        subprocess.run([os.environ.get("OPENSSL", "openssl"), "ca", "-batch", "-notext",
                        "-config", str(config), "-in", str(self.pki / "client.csr"),
                        "-startdate", "200101000000Z", "-enddate", "210101000000Z",
                        "-extfile", str(self.pki / "client.ext"), "-out", str(expired)],
                       check=True, capture_output=True)
        ctx = self.context(False)
        ctx.load_cert_chain(str(expired), str(self.pki / "client.key"))
        with self.server(extra_args=self.arguments()) as (_, connect):
            with self.assertRaises((ssl.SSLError, AssertionError, ConnectionResetError)):
                with ctx.wrap_socket(connect(), server_hostname="localhost") as client:
                    line(client)


if __name__ == "__main__":
    unittest.main()
