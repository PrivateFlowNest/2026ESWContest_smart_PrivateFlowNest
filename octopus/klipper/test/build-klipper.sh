#!/usr/bin/env bash
# Build Klipper for the STM32F446 Octopus (data dictionary for batch mode). Used by klipper-check.Dockerfile.
set -euo pipefail
export DEBIAN_FRONTEND=noninteractive
apt-get update -qq >/dev/null
apt-get install -y -qq --no-install-recommends git ca-certificates python3 python3-venv python3-dev \
  build-essential gcc-arm-none-eabi libnewlib-arm-none-eabi binutils-arm-none-eabi libffi-dev >/dev/null
git clone -q --depth 1 https://github.com/Klipper3d/klipper /klipper
cd /klipper
echo "klipper $(git rev-parse --short HEAD)"
cat > .config <<'EOF'
CONFIG_LOW_LEVEL_OPTIONS=y
CONFIG_MACH_STM32=y
CONFIG_MACH_STM32F446=y
CONFIG_STM32_FLASH_START_8000=y
CONFIG_STM32_CLOCK_REF_12M=y
CONFIG_STM32_USB_PA11_PA12=y
EOF
make olddefconfig >/dev/null
make -j"$(nproc)" >/dev/null
ls -la out/klipper.bin out/klipper.dict
python3 -m venv /venv
/venv/bin/pip install -q -r scripts/klippy-requirements.txt
