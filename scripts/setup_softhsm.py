#!/usr/bin/env python3
"""Disposable token with a newly generated, non-exportable RSA signing key."""
import os
from pathlib import Path
import secrets
import shutil
import subprocess
import sys

from create_test_pki import create, OPENSSL


def main():
    if len(sys.argv) != 2:
        raise SystemExit("Usage: python3 scripts/setup_softhsm.py /absolute/path/pkcs11.dylib")
    provider = Path(sys.argv[1]).resolve(strict=True)
    module = Path(os.environ.get("SOFTHSM_MODULE", "/opt/homebrew/lib/softhsm/libsofthsm2.so")).resolve(strict=True)
    os.umask(0o077)
    root = Path(__file__).resolve().parents[1]
    demo = root / ".softhsm-demo"
    demo.mkdir(mode=0o700)  # Refuse to overwrite existing tokens or keys.
    (demo / "tokens").mkdir()
    shutil.copy2(provider, demo / provider.name)
    (demo / "softhsm.conf").write_text(f"directories.tokendir = {demo / 'tokens'}\nobjectstore.backend = file\nlog.level = ERROR\n")
    # Provider v1.1 reads at most 31 characters from a PIN file.
    pin = secrets.token_hex(12)
    (demo / "pin.txt").write_text(pin + "\n")
    (demo / "openssl.cnf").write_text(
        "openssl_conf = init\n[init]\nproviders = providers\n[providers]\n"
        "default = default_section\npkcs11 = token_section\n[default_section]\nactivate = 1\n"
        f"[token_section]\nmodule = {demo / provider.name}\npkcs11-module-path = {module}\n"
        f"pkcs11-module-token-pin = file:{demo / 'pin.txt'}\nactivate = 1\n")
    env = dict(os.environ, SOFTHSM2_CONF=str(demo / "softhsm.conf"),
               DEMO_PIN=pin, DEMO_SO_PIN=secrets.token_hex(16))

    def token(*args):
        subprocess.run(["pkcs11-tool", "--module", str(module), *args], env=env, check=True)

    # env: avoids putting either PIN in process arguments or shell history.
    token("--init-token", "--slot-index", "0", "--label", "tcp-demo", "--so-pin", "env:DEMO_SO_PIN")
    token("--token-label", "tcp-demo", "--login", "--login-type", "so", "--so-pin", "env:DEMO_SO_PIN",
          "--init-pin", "--pin", "env:DEMO_PIN")
    token("--token-label", "tcp-demo", "--login", "--pin", "env:DEMO_PIN", "--keypairgen",
          "--key-type", "RSA:2048", "--id", "01", "--label", "server", "--usage-sign", "--sensitive")
    pki = root / ".test-pki"
    if not pki.exists():
        create(pki)
    uri = "pkcs11:token=tcp-demo;object=server;type=private"
    env["OPENSSL_CONF"] = str(demo / "openssl.cnf")
    subprocess.run([OPENSSL, "req", "-new", "-key", uri, "-subj", "/CN=localhost",
                    "-out", str(demo / "server.csr")], env=env, check=True)
    subprocess.run([OPENSSL, "x509", "-req", "-in", str(demo / "server.csr"), "-CA", str(pki / "ca.crt"),
                    "-CAkey", str(pki / "ca.key"), "-CAcreateserial", "-days", "1",
                    "-extfile", str(pki / "server.ext"), "-out", str(demo / "server.crt")], check=True)
    print("Token and certificate created. See docs/CRYPTOGRAPHY.md to start the server.")


if __name__ == "__main__":
    main()
