#!/usr/bin/env python3
"""Create an isolated, disposable CA and certificates. Never use for production."""
import os
from pathlib import Path
import subprocess
import sys

OPENSSL = os.environ.get("OPENSSL", "openssl")


def run(*args):
    subprocess.run([OPENSSL, *map(str, args)], check=True, stdout=subprocess.DEVNULL,
                   stderr=subprocess.PIPE)


def create(directory):
    os.umask(0o077)
    directory = Path(directory)
    directory.mkdir(mode=0o700, parents=True, exist_ok=False)
    run("req", "-x509", "-newkey", "rsa:2048", "-noenc", "-days", "2",
        "-subj", "/CN=Disposable Lab CA", "-keyout", directory / "ca.key",
        "-out", directory / "ca.crt", "-addext", "basicConstraints=critical,CA:TRUE",
        "-addext", "keyUsage=critical,keyCertSign,cRLSign")
    for name, purpose in (("server", "serverAuth"), ("client", "clientAuth")):
        run("req", "-new", "-newkey", "rsa:2048", "-noenc", "-subj", f"/CN={name}",
            "-keyout", directory / f"{name}.key", "-out", directory / f"{name}.csr")
        extensions = directory / f"{name}.ext"
        extensions.write_text("basicConstraints=critical,CA:FALSE\n"
                              "keyUsage=critical,digitalSignature\n"
                              f"extendedKeyUsage={purpose}\n" +
                              ("subjectAltName=DNS:localhost,IP:127.0.0.1\n" if name == "server" else ""))
        run("x509", "-req", "-in", directory / f"{name}.csr", "-CA", directory / "ca.crt",
            "-CAkey", directory / "ca.key", "-CAcreateserial", "-days", "1",
            "-extfile", extensions, "-out", directory / f"{name}.crt")


if __name__ == "__main__":
    create(sys.argv[1] if len(sys.argv) > 1 else ".test-pki")
