#!/usr/bin/env bash
set -euo pipefail

# Small smoke test for the TCP server.
# Useful shell patterns in one place:
# - default arguments with ${VAR:-default}
# - functions
# - command substitution $(...)
# - trap cleanup on exit
# - simple checks with grep

HOST="${1:-127.0.0.1}"
PORT="${2:-9000}"
SERVER_BIN="${SERVER_BIN:-./build/tcp_server}"
SERVER_PID=""

cleanup() {
  if [[ -n "${SERVER_PID}" ]] && kill -0 "${SERVER_PID}" 2>/dev/null; then
    kill "${SERVER_PID}"
    wait "${SERVER_PID}" 2>/dev/null || true
  fi
}

wait_for_server() {
  local attempts=20

  for ((i = 1; i <= attempts; i++)); do
    if printf 'PING\nQUIT\n' | nc "${HOST}" "${PORT}" | grep -q 'PONG'; then
      return 0
    fi
    sleep 0.1
  done

  echo "server did not become ready on ${HOST}:${PORT}" >&2
  return 1
}

send_commands() {
  printf '%s\n' \
    'PING' \
    'ECHO hello from shell' \
    '{"command":"ECHO","text":"hello from json"}' \
    'STATS' \
    'QUIT' | nc "${HOST}" "${PORT}"
}

assert_contains() {
  local output="$1"
  local expected="$2"

  if ! grep -q "${expected}" <<<"${output}"; then
    echo "expected output to contain: ${expected}" >&2
    echo "actual output:" >&2
    echo "${output}" >&2
    return 1
  fi
}

if [[ ! -x "${SERVER_BIN}" ]]; then
  echo "server binary not found: ${SERVER_BIN}" >&2
  echo "run: make" >&2
  exit 1
fi

trap cleanup EXIT

"${SERVER_BIN}" --host "${HOST}" --port "${PORT}" >/tmp/tcp_interview_server.log 2>&1 &
SERVER_PID="$!"

wait_for_server

OUTPUT="$(send_commands)"

echo "${OUTPUT}"

assert_contains "${OUTPUT}" 'PONG'
assert_contains "${OUTPUT}" 'hello from shell'
assert_contains "${OUTPUT}" 'hello from json'
assert_contains "${OUTPUT}" 'commands='
assert_contains "${OUTPUT}" 'BYE'

echo "smoke test passed"
