# Cross-build llama.cpp (llama-server, llama-cli) and whisper.cpp (whisper-cli, whisper-server) for aarch64
# (Raspberry Pi 5, Cortex-A76). Ubuntu 24.04 = glibc 2.39, same as LG AI Native OS (Yocto scarthgap).
# docker buildx build -f native/arm64-tools/Dockerfile --build-context llama=third_party/llama.cpp \
#   --build-context whisper=services/edge-ai/speech-to-text/models/whisper.cpp-b4938-source/whisper.cpp-b4938 \
#   --output type=local,dest=native/.deps/arm64-tools .
FROM ubuntu:24.04 AS build
RUN apt-get update && DEBIAN_FRONTEND=noninteractive apt-get install -y --no-install-recommends \
    cmake ninja-build git ca-certificates g++ g++-aarch64-linux-gnu gcc-aarch64-linux-gnu file \
    && rm -rf /var/lib/apt/lists/*
COPY aarch64.cmake /toolchain.cmake
COPY --from=llama . /src/llama
COPY --from=whisper . /src/whisper
ARG CPU_FLAGS="-mcpu=cortex-a76"
RUN cmake -S /src/llama -B /b/llama -G Ninja -DCMAKE_TOOLCHAIN_FILE=/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
      -DGGML_NATIVE=OFF -DGGML_CPU_ARM_ARCH=armv8.2-a+dotprod+fp16 -DLLAMA_CURL=OFF -DLLAMA_BUILD_HTML=OFF -DBUILD_SHARED_LIBS=OFF \
      -DLLAMA_BUILD_TESTS=OFF -DLLAMA_BUILD_EXAMPLES=OFF -DLLAMA_BUILD_SERVER=ON -DLLAMA_BUILD_TOOLS=ON \
      -DCMAKE_C_FLAGS="${CPU_FLAGS}" -DCMAKE_CXX_FLAGS="${CPU_FLAGS}" \
 && cmake --build /b/llama --target llama-server llama-cli -j"$(nproc)"
RUN cmake -S /src/whisper -B /b/whisper -G Ninja -DCMAKE_TOOLCHAIN_FILE=/toolchain.cmake -DCMAKE_BUILD_TYPE=Release \
      -DGGML_NATIVE=OFF -DBUILD_SHARED_LIBS=OFF -DWHISPER_BUILD_TESTS=OFF -DWHISPER_BUILD_EXAMPLES=ON \
      -DCMAKE_C_FLAGS="${CPU_FLAGS}" -DCMAKE_CXX_FLAGS="${CPU_FLAGS}" \
 && cmake --build /b/whisper -j"$(nproc)" \
 && mkdir -p /out/bin \
 && cp /b/llama/bin/llama-server /b/llama/bin/llama-cli /out/bin/ \
 && (cp /b/whisper/bin/whisper-cli /out/bin/ 2>/dev/null || cp /b/whisper/bin/main /out/bin/whisper-cli) \
 && (cp /b/whisper/bin/whisper-server /out/bin/ 2>/dev/null || cp /b/whisper/bin/server /out/bin/whisper-server || true) \
 && aarch64-linux-gnu-strip /out/bin/* && file /out/bin/* > /out/FILES.txt
FROM scratch
COPY --from=build /out/ /
