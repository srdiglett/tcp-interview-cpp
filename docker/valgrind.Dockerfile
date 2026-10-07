FROM debian:bookworm-slim
RUN apt-get update && apt-get install -y --no-install-recommends \
    g++ libc6-dbg valgrind && rm -rf /var/lib/apt/lists/*
WORKDIR /work
