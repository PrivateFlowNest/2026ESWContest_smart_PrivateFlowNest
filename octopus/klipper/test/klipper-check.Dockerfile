# Klipper batch-mode checker image (PC simulation only): Klipper built for the STM32F446 Octopus.
#   docker build -t homemind-klipper-check -f octopus/klipper/test/klipper-check.Dockerfile octopus/klipper/test
FROM ubuntu:24.04
COPY build-klipper.sh /build-klipper.sh
RUN bash /build-klipper.sh
