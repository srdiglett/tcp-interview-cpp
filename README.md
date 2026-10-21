# TCP Interview C++

Small TCP server in modern C++ for interview practice.

The goal is to keep the code simple enough to explain at a whiteboard, while covering sockets, bind/listen/accept, line-based protocols, a bounded connection queue, a fixed worker pool, coordinated shutdown, and tests.

See [Producer-consumer and mutex guide](docs/CONCURRENCY.md) for the current
architecture and a three-client demo. The PowerPoint in docs/ covers the current
worker pool and includes the Yocto/QEMU demo as an appendix.

## Study Guide

For a guided explanation of the project, methods, shell script, SFTP example, and UX analytics example, read:

```text
docs/STUDY_NOTES.md
```

## Build

With Make, no extra dependencies beyond Apple clang/Xcode Command Line Tools:

```bash
make
```

With CMake and Python 3 installed (Python runs the server integration tests):

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
./build/tcp_server --host 127.0.0.1 --port 9000
```

Pool settings: `--workers 4 --queue-capacity 16`. A full queue rejects new
connections; each worker handles one connection until it closes.

In another terminal:

```bash
nc 127.0.0.1 9000
```

## Commands

Plain text commands still work:

- `PING`: returns `PONG`.
- `ECHO <text>`: returns `<text>`.
- `TIME`: returns server UTC time.
- `STATS`: returns total connections, active connections, and command count.
- `HELP`: lists commands.
- `QUIT`: closes the client connection.

JSON Lines also work. Each JSON object must fit on one line and end with `\n`:

```bash
printf '{"command":"PING"}\n' | nc 127.0.0.1 9000
printf '{"command":"ECHO","text":"hola desde json"}\n' | nc 127.0.0.1 9000
printf '{"command":"QUIT"}\n' | nc 127.0.0.1 9000
```

This project intentionally includes a tiny flat-object JSON parser for interview practice. It supports string fields like `command` and `text`; a production server should use a real JSON library.

## Shell Script Smoke Test

There is a small Bash script that starts the server, checks it with `nc`, and stops it automatically:

```bash
make smoke
```

The script is in `scripts/smoke_test_tcp.sh` and demonstrates common shell patterns: `set -euo pipefail`, variables, functions, command substitution, `trap`, exit checks, and piping data into `nc`.

## UX Analytics Example

There is a standalone Boost.JSON example that reads telecom-style JSON Lines events from stdin, aggregates QoE metrics per cell, and emits alerts:

```bash
make ux-analytics
./build/ux_analytics_pipeline < data/sample_network_events.jsonl
```

The example models a common analytics shape: parse events, validate fields, group by `cell_id`, compute latency/throughput/loss averages, classify QoE, and print degraded cells.

## SFTP/SSH Integration Example

For a real local SFTP target running Yocto Linux ARM64 with its own kernel in
QEMU, see [Yocto/QEMU demo](docs/YOCTO_QEMU.md). The client accepts an optional
final port argument, for example `2222` for this virtual machine.

The server in this repository is still a plain TCP learning server. For real SFTP/SSH usage in C++, see:

```text
examples/sftp_upload_libssh2.cpp
```

That example shows the normal production shape: open a TCP connection to port 22, start an SSH session with `libssh2`, inspect the host key, authenticate with a private key, open the SFTP subsystem, and upload a file.

Install dependency on macOS:

```bash
brew install libssh2
```

Build the optional example:

```bash
make sftp-example
```

Run it:

```bash
./build/sftp_upload_libssh2 sftp.example.com my_user ~/.ssh/id_ed25519 ./local.txt /remote/local.txt
```

Interview angle: you would not implement SSH/SFTP from zero. You would integrate a tested library and be careful with host key verification, authentication method, timeouts, retries, partial writes, remote paths, and cleanup.

## Tests

GitHub Actions builds and tests the core C++ server on pushes and pull requests.
An opt-in SonarQube Cloud job analyzes the default branch and checks its quality
gate. See [CI and SonarQube setup](docs/CI_SONARQUBE.md) for account configuration,
repository variables, scope, and the difference between CI/CD, Actions and Jenkins.

For a commented Valgrind practice using Linux in Docker, see
[docs/VALGRIND.md](docs/VALGRIND.md). Run `make valgrind` for protocol tests
or `make valgrind-server` for an interactive TCP session.

With Make:

```bash
make test
make test-server
```

With CMake:

```bash
ctest --test-dir build --output-on-failure
```

## Interview Talking Points

- Why TCP is stream-oriented, not message-oriented.
- Why this server defines a line-based protocol with `\n` delimiters.
- Producer-consumer coordination with mutex, condition_variable and join.
- Tradeoffs of a blocking worker pool versus an event loop.
- Handling partial reads/writes and client disconnects.
- Where you would add timeouts, TLS, structured logging, and backpressure.
