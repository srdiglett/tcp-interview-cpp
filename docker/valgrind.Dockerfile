FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ libc6-dbg valgrind libssl-dev && rm -rf /var/lib/apt/lists/*
WORKDIR /work
