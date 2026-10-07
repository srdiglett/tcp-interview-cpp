#!/usr/bin/env bash
set -euo pipefail

# Resuelve el proyecto aunque ejecutes el script desde otro directorio.
root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mode="${1:-tests}"
case "$mode" in
  tests|server|bug|fixed) ;;
  *) echo "Uso: bash scripts/valgrind.sh [tests|server|bug|fixed]" >&2; exit 2 ;;
esac

if ! docker info >/dev/null 2>&1; then
  echo "Arranca Docker Desktop y vuelve a ejecutar el comando." >&2
  exit 1
fi
docker build -t tcp-interview-valgrind -f "$root/docker/valgrind.Dockerfile" "$root/docker"

# Montaje de solo lectura: los binarios Linux se crean en /tmp del contenedor.
args=(--rm --init -v "$root:/work:ro")
if [[ "$mode" == server ]]; then
  args+=(-it -p 127.0.0.1:9000:9000)
fi
docker run "${args[@]}" tcp-interview-valgrind bash -euc '
  mode="$1"
  flags=(-std=c++17 -Wall -Wextra -g -O0 -pthread -Iinclude)
  program_args=()
  case "$mode" in
    tests) sources=(tests/protocol_tests.cpp src/protocol.cpp) ;;
    server)
      sources=(src/main.cpp src/server.cpp src/protocol.cpp)
      program_args=(--host 0.0.0.0 --port 9000)
      ;;
    bug|fixed)
      sources=(examples/valgrind_memory.cpp)
      program_args=("$mode")
      ;;
  esac
  g++ "${flags[@]}" "${sources[@]}" -o /tmp/valgrind-program
  exec valgrind --tool=memcheck --leak-check=full --show-leak-kinds=all \
    --track-origins=yes --track-fds=yes --error-exitcode=99 \
    /tmp/valgrind-program "${program_args[@]}"
' bash "$mode"
