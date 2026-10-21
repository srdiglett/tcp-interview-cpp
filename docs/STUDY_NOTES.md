# Study Notes

This document summarizes what we built and the concepts worth explaining in an interview.

The project has four separate learning areas:

- A plain TCP server in C++.
- A tiny line-based protocol with optional JSON Lines commands.
- A shell smoke test that drives the server with `nc`.
- Two realistic examples: SFTP integration with `libssh2`, and UX/QoE analytics with Boost.JSON.

## Project Map

```text
include/tcp_interview/protocol.hpp      public protocol types and LineProtocol class
include/tcp_interview/server.hpp        public TCP server config and TcpServer class
src/protocol.cpp                        command parsing and command execution
src/server.cpp                          socket(), bind(), listen(), accept(), recv(), send()
src/main.cpp                            CLI entrypoint: --host and --port
tests/protocol_tests.cpp                simple C++ assertions for protocol behavior
scripts/smoke_test_tcp.sh               Bash test that starts the server and uses nc
examples/sftp_upload_libssh2.cpp        realistic SFTP upload using libssh2
examples/ux_analytics_pipeline.cpp      telecom-style UX analytics pipeline with Boost.JSON
data/sample_network_events.jsonl        sample network events for the analytics example
Makefile                                build targets
CMakeLists.txt                          alternative CMake build definition
```

## How To Build And Run

Build the server:

```bash
make
```

Run the server:

```bash
./build/tcp_server --host 127.0.0.1 --port 9000
```

Connect manually with netcat:

```bash
nc 127.0.0.1 9000
```

Try commands:

```text
PING
ECHO hello
{"command":"ECHO","text":"hello json"}
STATS
QUIT
```

Run tests:

```bash
make test
make smoke
```

Run the UX analytics example:

```bash
make ux-analytics
./build/ux_analytics_pipeline < data/sample_network_events.jsonl
```

Build the optional SFTP example:

```bash
make sftp-example
```

## TCP Server Flow

Main files:

- `src/main.cpp`
- `include/tcp_interview/server.hpp`
- `src/server.cpp`

Typical server flow:

```text
socket()  -> create a TCP socket
bind()    -> attach socket to local IP and port
listen()  -> mark it as a passive listening socket
accept()  -> wait for a client connection
recv()    -> read bytes from the client
send()    -> write bytes to the client
close()   -> release the descriptor
```

### `ServerConfig`

Defined in `server.hpp`:

```cpp
struct ServerConfig {
    std::string host{"127.0.0.1"};
    std::uint16_t port{9000};
    std::size_t max_line_bytes{4096};
    std::size_t worker_count{4};
    std::size_t queue_capacity{16};
};
```

What to say:

> `ServerConfig` groups runtime settings: bind address, port, and maximum accepted line size. Keeping this separate makes the server easier to configure and test.

### `TcpServer::run()`

Defined in `src/server.cpp`.

Responsibilities:

- Create the listening socket with `socket(AF_INET, SOCK_STREAM, 0)`.
- Enable `SO_REUSEADDR` to allow quick restart after stopping the process.
- Convert the host string into a binary IPv4 address with `inet_pton`.
- Bind the socket to `host:port`.
- Put the socket into listening mode.
- Enter the accept loop.

Key interview point:

> `bind` assigns the local address and port to the server socket. `listen` then tells the kernel this socket should accept incoming TCP connections.

### `TcpServer::accept_loop()`

Responsibilities:

- Wait with `poll()` and accept a client using a nonblocking listening socket.
- Receive a new `client_fd` for that connection.
- Enqueue the socket for a fixed pool of workers, or close it if the queue is full.

Current design:

```cpp
clients_ready_.wait(lock, [this] {
    return stopping_ || !pending_clients_.empty();
});
```

What to say:

> The producer accepts connections and workers consume them from a bounded queue. The mutex protects the queue and connection registry, not network processing. Idle clients still occupy workers. See CONCURRENCY.md for the complete lifecycle, shutdown and demo.

### `TcpServer::handle_client()`

Responsibilities:

- Increment connection counters.
- Send a greeting.
- Read one line at a time.
- Pass each line to `LineProtocol`.
- Send the response back.
- Return on `QUIT`, disconnect or I/O error; the worker closes the connection.

Important idea:

> TCP is a byte stream, not a message protocol. The server defines its own framing rule: one command equals one line ending in `\n`.

### `read_line()`

This helper reads from the socket byte by byte until `\n` or until the maximum line size is reached.

Why it exists:

> With TCP, one `send()` from the client is not guaranteed to match one `recv()` on the server. You need a framing strategy: newline-delimited messages, fixed-size headers, length prefixes, etc.

### `send_all()`

This helper keeps calling `send()` until the whole response has been written.

Why it matters:

> `send()` can write only part of the buffer. Robust socket code handles partial writes.

## Protocol Flow

Main files:

- `include/tcp_interview/protocol.hpp`
- `src/protocol.cpp`
- `tests/protocol_tests.cpp`

Supported text commands:

```text
PING
ECHO <text>
TIME
STATS
HELP
QUIT
```

Supported JSON Lines commands:

```json
{"command":"PING"}
{"command":"ECHO","text":"hello"}
{"command":"QUIT"}
```

### `ServerStats`

Defined in `protocol.hpp`:

```cpp
struct ServerStats {
    std::atomic<int> total_connections{0};
    std::atomic<int> active_connections{0};
    std::atomic<int> total_commands{0};
};
```

What `std::atomic<int>` means:

> These counters can be incremented safely from multiple threads. A plain `int++` is not safe when several client threads update it at the same time.

When atomic is enough:

- Simple counters.
- Flags.
- Values updated independently.

When atomic is not enough:

- Protecting a `std::map`.
- Updating several variables that must stay consistent together.
- Complex state transitions.

For those cases, use a `std::mutex` or a more explicit concurrency design.

### `LineProtocol::handle()`

High-level entrypoint.

Responsibilities:

- Trim the incoming line.
- Reject empty commands.
- Increment command counter.
- Dispatch to text command parser or JSON command parser.

Interview sentence:

> `handle()` is the boundary between transport and application protocol: the TCP layer gives us a line of text, and the protocol layer decides what command it means.

### `handle_text_command()`

Parses commands like:

```text
ECHO hello world
```

It splits the first word as the command and treats the rest as the argument.

### `handle_json_command()`

Parses commands like:

```json
{"command":"ECHO","text":"hello"}
```

It uses a tiny hand-written parser in this project. That is useful for learning, but not recommended for production.

Production answer:

> In real code I would use a tested JSON library such as Boost.JSON, nlohmann/json, RapidJSON, or simdjson, depending on project needs and performance requirements.

### `execute_command()`

Runs the actual command after parsing.

Examples:

- `PING` -> `PONG`
- `ECHO hello` -> `hello`
- `TIME` -> UTC timestamp
- `STATS` -> connection and command counters
- `QUIT` -> `BYE` and closes connection

This separation is useful because text and JSON inputs can share the same command execution logic.

## Shell Script Notes

Main file:

- `scripts/smoke_test_tcp.sh`

Run:

```bash
make smoke
```

### `set -euo pipefail`

Meaning:

- `-e`: stop when a command fails.
- `-u`: fail when using an undefined variable.
- `-o pipefail`: a pipeline fails if any command in it fails.

Interview/useful sentence:

> It makes scripts fail early instead of continuing in a half-broken state.

### `${VAR}`

Expands a variable safely and clearly.

Example:

```bash
echo "${HOST}:${PORT}"
```

### `${1:-default}`

Use an argument if present; otherwise use a default.

Example from the script:

```bash
HOST="${1:-127.0.0.1}"
PORT="${2:-9000}"
```

### `trap cleanup EXIT`

Runs `cleanup` when the script exits, even if it fails.

Why it matters here:

> The script starts a background server. `trap` ensures the server is stopped at the end.

### `>&2`

Sends output to stderr.

Example:

```bash
echo "error message" >&2
```

Use stdout for normal output and stderr for diagnostics/errors.

### `<<<"${output}"`

This is a here-string. It passes the contents of a variable as stdin to a command.

Example:

```bash
grep -q "PONG" <<<"${output}"
```

Equivalent idea:

```bash
echo "${output}" | grep -q "PONG"
```

## SFTP/SSH Example

Main file:

- `examples/sftp_upload_libssh2.cpp`

This example is intentionally separate from the toy TCP server.

What it demonstrates:

```text
connect TCP to host:22
start SSH session
inspect host key fingerprint
authenticate with private key
open SFTP subsystem
upload a local file
close SFTP/session/socket
```

Important interview answer:

> I would not implement SSH or SFTP from scratch. I would integrate a proven library such as libssh2/libssh, validate the host key, authenticate with key or password, configure timeouts/retries, and handle partial reads/writes and cleanup.

### `reinterpret_cast`

Used in the SFTP example:

```cpp
const unsigned char* hash = reinterpret_cast<const unsigned char*>(
    libssh2_hostkey_hash(session, LIBSSH2_HOSTKEY_HASH_SHA1));
```

Meaning:

> `reinterpret_cast` tells C++ to treat the same memory address as a different pointer type. It does not transform the data. It is common around C APIs and raw bytes, but should be used carefully.

### RAII `Fd` wrapper

The example wraps the socket file descriptor in a small class:

```cpp
class Fd {
public:
    explicit Fd(int value = -1);
    ~Fd();
};
```

Why:

> RAII means the resource is released in the destructor. When the object goes out of scope, the file descriptor is closed automatically.

## UX Analytics Example

Main files:

- `examples/ux_analytics_pipeline.cpp`
- `data/sample_network_events.jsonl`

Input shape:

```json
{"timestamp":"2026-09-08T09:00:00Z","cell_id":"MAD-001","subscriber_id":"u-100","app":"video","latency_ms":42.0,"throughput_mbps":38.5,"packet_loss_pct":0.1}
```

Pipeline:

```text
read line from stdin
parse JSON with Boost.JSON
validate required fields
convert to NetworkEvent
aggregate into CellWindowStats per cell_id
classify QoE
print report and alerts
```

### `NetworkEvent`

Represents one raw telecom/network event after parsing.

Fields:

- `timestamp`
- `cell_id`
- `subscriber_id`
- `app`
- `latency_ms`
- `throughput_mbps`
- `packet_loss_pct`

Interview sentence:

> This is the normalized event model. Real systems often normalize raw vendor/probe formats into internal event structures before aggregating.

### `CellWindowStats`

Aggregates events for one cell.

It keeps:

- sample count;
- latency sum;
- throughput sum;
- packet loss sum;
- counters for bad samples.

Methods:

- `add(event)`: adds one event to the aggregate.
- `avg_latency_ms()`: average latency.
- `avg_throughput_mbps()`: average throughput.
- `avg_packet_loss_pct()`: average packet loss.

Interview sentence:

> This mimics a time-window aggregation. In production, the window could be one minute, five minutes, or streaming windows in Kafka/Flink/Spark/etc.

### `parse_event_line()`

Uses Boost.JSON:

```cpp
const json::value parsed = json::parse(line);
```

Then validates fields and returns:

```cpp
std::optional<NetworkEvent>
```

Why `optional`:

> A malformed event should not necessarily stop the whole pipeline. We can drop it, log the error, and continue processing the rest.

### `classify_qoe()`

Turns metrics into a status:

```text
GOOD
DEGRADED
CRITICAL
```

Current simplified thresholds:

- high latency: average latency > 100 ms;
- high loss: average packet loss > 1%;
- low throughput: average throughput < 8 Mbps.

Interview sentence:

> The thresholds are toy values. In real UX analytics they would depend on service type, access technology, customer segment, device, geography, and SLA/SLO.

### `print_alerts()`

Prints only degraded or critical cells.

Why this matters:

> Analytics is useful when it becomes operational: alerts, dashboards, troubleshooting hints, or automated remediation.

## Telecom Vocabulary To Recognize

- RAN: Radio Access Network, the radio part of the mobile network.
- Cell: coverage area served by a radio node/antenna sector.
- QoS: Quality of Service, network/service-level metrics.
- QoE: Quality of Experience, user-perceived quality.
- Latency: time delay.
- Throughput: data rate.
- Packet loss: packets that do not arrive.
- Jitter: variation in latency.
- Handover: moving a user session from one cell to another.
- Backpressure: mechanism to avoid accepting data faster than it can be processed.

## Good Interview Phrases

Protocols:

> I have worked with protocols in payments/contactless, including EMV, Mifare, ISO 8583, PKCS#11/OpenSSL, and proprietary protocols. I am used to reading specifications, validating message formats, handling states/errors, and debugging integrations with real devices.

TCP:

> TCP gives me an ordered byte stream, but not application-level messages. The application must define framing, such as newline-delimited messages or length-prefixed frames.

Boost:

> I know Boost as a common C++ ecosystem in enterprise code. For this kind of work, Boost.Asio is relevant for networking and Boost.JSON for JSON parsing. If performance is the priority, I would also consider simdjson.

SFTP/SSH:

> I would not implement SSH from scratch. I would use a proven library, verify the host key, authenticate correctly, handle timeouts/retries, and treat file operations as partial/failable operations.

UX analytics/telco:

> I understand UX analytics here as network user-experience analytics: ingesting large volumes of events from network elements, aggregating them by cell/user/service/time window, and producing actionable signals like degradation, anomaly alerts, or troubleshooting context.

Your background:

> My background is not pure telecom, but it is close in engineering shape: C++ on Linux/ARM, devices, protocols, cryptography, certification, and debugging real integrations. That gives me a strong base to learn the telco domain quickly.

## Things To Review Before The Interview

- Explain `socket`, `bind`, `listen`, `accept`, `recv`, `send`.
- Explain TCP stream vs UDP datagrams.
- Explain framing with `\n` and why partial reads/writes matter.
- Explain `std::atomic<int>` vs `std::mutex`.
- Explain why JSON should be parsed with a real library.
- Explain what Boost is and name Boost.Asio / Boost.JSON.
- Explain SSH vs SFTP vs FTPS.
- Explain QoE metrics: latency, throughput, packet loss, jitter.
- Explain one project from your CV involving C++, Linux, protocols, and devices.
