# Build/test image for native/ (x86_64 PC, or --platform linux/arm64 under QEMU for the Pi5 build).
#   docker build -t homemind-native-dev:24.04 -f native/docker/dev.Dockerfile native/.deps/downloads/onnxruntime-1.20.1
FROM ubuntu:24.04
ARG TARGETARCH
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    build-essential cmake ninja-build pkg-config git ca-certificates file \
    libmosquitto-dev mosquitto mosquitto-clients nlohmann-json3-dev libutf8proc-dev libpcre2-dev \
    libasound2-dev libgtest-dev python3 libjpeg-turbo8-dev libpng-dev libsystemd-dev libssl-dev \
    && rm -rf /var/lib/apt/lists/*
# ONNX Runtime 1.20.1 prebuilt (SHA-256 in docs/handoff/MODEL_MANIFEST.md) -> /opt/onnxruntime
COPY onnxruntime-linux-*-1.20.1.tgz /tmp/ort/
RUN set -eu; case "${TARGETARCH:-amd64}" in amd64) a=x64;; arm64) a=aarch64;; *) exit 1;; esac; \
    tar -xzf /tmp/ort/onnxruntime-linux-$a-1.20.1.tgz -C /opt; mv /opt/onnxruntime-linux-$a-1.20.1 /opt/onnxruntime; \
    rm -rf /tmp/ort; ls /opt/onnxruntime/lib
ENV HM_ONNXRUNTIME_ROOT=/opt/onnxruntime
