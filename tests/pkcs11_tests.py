"""Opt-in end-to-end test using the locally provisioned SoftHSM demo."""
from concurrent.futures import ThreadPoolExecutor
import os
from pathlib import Path
import ssl
import subprocess
import unittest
from unittest.mock import patch

import server_pool_tests
from server_pool_tests import line, BINARY


class TokenTests(unittest.TestCase):
    server = server_pool_tests.PoolTests.server

    def test_concurrent_token_handshakes(self):
        root = Path(__file__).resolve().parents[1]
        demo = root / ".softhsm-demo"
        pki = root / ".test-pki"
        self.assertTrue((demo / "server.crt").exists(), "Run scripts/setup_softhsm.py first")
        env = {"OPENSSL_CONF": str(demo / "openssl.cnf"),
               "SOFTHSM2_CONF": str(demo / "softhsm.conf")}
        args = ["--tls-cert", str(demo / "server.crt"),
                "--tls-key", "pkcs11:token=tcp-demo;object=server;type=private",
                "--tls-ca", str(pki / "ca.crt")]
        context = ssl.create_default_context(cafile=str(pki / "ca.crt"))
        context.load_cert_chain(str(pki / "client.crt"), str(pki / "client.key"))
        with patch.dict(os.environ, env):
            with self.server(workers=4, capacity=16, extra_args=args) as (_, connect):
                def exchange(_):
                    with context.wrap_socket(connect(), server_hostname="localhost") as client:
                        self.assertIn(b"ready", line(client))
                        client.sendall(b"PING\nQUIT\n")
                        self.assertEqual(line(client), b"PONG")
                        self.assertEqual(line(client), b"BYE")
                with ThreadPoolExecutor(max_workers=8) as workers:
                    list(workers.map(exchange, range(16)))
            args[3] = "pkcs11:token=tcp-demo;object=missing;type=private"
            result = subprocess.run([BINARY, *args], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 1)
        # No configured provider must fail closed, not load a PEM fallback.
        args[3] = "pkcs11:token=tcp-demo;object=server;type=private"
        with patch.dict(os.environ, {"OPENSSL_CONF": os.devnull}):
            result = subprocess.run([BINARY, *args], capture_output=True, timeout=5)
            self.assertEqual(result.returncode, 1)


if __name__ == "__main__":
    unittest.main()
