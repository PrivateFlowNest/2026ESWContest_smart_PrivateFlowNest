# syntax=docker/dockerfile:1
# HomeMind 7-inch UI (NODE-RPI5-01) built with open-source flutter-elinux, without the Atlas SDK.
# PC simulation only. See tool/elinux/build_elinux.sh (host) and README_KO.md "eLinux 번들".
#
#   builder : Ubuntu 24.04 x86_64. clang + lld cross-compile to aarch64 against Ubuntu arm64
#             multiarch packages (sysroot = /). Also builds the x64 bundle for the PC test.
#   runtime : Ubuntu 24.04 (amd64 or arm64 via --platform). weston headless (pixman) +
#             Mesa software EGL/GLES, to start the bundle and take a screenshot.
#
# Only Ubuntu packages are installed here. flutter-elinux, the Flutter SDK it pins and the
# eLinux engine artifacts are fetched by build_in_container.sh into a docker volume.

FROM ubuntu:24.04 AS builder
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN set -eux; \
    sed -i '/^Components:/a Architectures: amd64' /etc/apt/sources.list.d/ubuntu.sources; \
    printf '%s\n' 'Types: deb' 'URIs: http://ports.ubuntu.com/ubuntu-ports/' \
      'Suites: noble noble-updates noble-security' 'Components: main universe' \
      'Architectures: arm64' 'Signed-By: /usr/share/keyrings/ubuntu-archive-keyring.gpg' \
      > /etc/apt/sources.list.d/ubuntu-ports-arm64.sources; \
    dpkg --add-architecture arm64; \
    printf '%s\n' 'Acquire::Retries "5";' 'Acquire::http::Pipeline-Depth "0";' \
      > /etc/apt/apt.conf.d/80retries; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      ca-certificates git curl unzip xz-utils zip file rsync python3 \
      clang lld cmake ninja-build make pkg-config binutils-aarch64-linux-gnu \
      libc6-dev libstdc++-13-dev \
      libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 libegl1 libgles2 \
      libfontconfig1 \
      libc6-dev:arm64 libstdc++-13-dev:arm64 libgcc-13-dev:arm64 \
      libwayland-client0:arm64 libwayland-cursor0:arm64 libwayland-egl1:arm64 \
      libxkbcommon0:arm64 libegl1:arm64 libgles2:arm64 libfontconfig1:arm64; \
    rm -rf /var/lib/apt/lists/*; \
    git config --system --add safe.directory '*'
WORKDIR /work

FROM ubuntu:24.04 AS runtime
ENV DEBIAN_FRONTEND=noninteractive LANG=C.UTF-8
RUN set -eux; \
    printf '%s\n' 'Acquire::Retries "5";' 'Acquire::http::Pipeline-Depth "0";' \
      > /etc/apt/apt.conf.d/80retries; \
    apt-get update; \
    apt-get install -y --no-install-recommends \
      weston xkb-data libwayland-client0 libwayland-cursor0 libwayland-egl1 libxkbcommon0 \
      libegl1 libgles2 libegl-mesa0 libgl1-mesa-dri libfontconfig1 file; \
    rm -rf /var/lib/apt/lists/*
WORKDIR /run-ui
