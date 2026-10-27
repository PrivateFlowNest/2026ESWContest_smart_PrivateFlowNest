# PC (x86_64) build and test image with the same Ubuntu 24.04 packages as ../../Dockerfile.
# docker build -t homemind-dev:24.04 -f tools/docker/dev.Dockerfile tools/docker
FROM ubuntu:24.04
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config mosquitto mosquitto-clients libmosquitto-dev \
    libsqlite3-dev nlohmann-json3-dev python3 sqlite3 ca-certificates \
    && rm -rf /var/lib/apt/lists/*
