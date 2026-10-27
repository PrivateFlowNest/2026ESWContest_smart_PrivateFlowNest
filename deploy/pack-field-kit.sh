#!/usr/bin/env bash
# Field kit for installing from another laptop (macOS, Linux or Git Bash), made on the PC that collected and signed
# the payloads. Contents: the committed code as one git bundle, the signed payloads (gitignored) as one tar, the
# site addresses and a Korean step list. No private key, password, token or secrets.h goes into the kit.
#   bash deploy/pack-field-kit.sh [--nodes "NODE-RPI4-01 …"] [--archive] [out dir]
#     default: every payload, out dir ~/Desktop/HomeMind-field-kit
#     --nodes    only these payloads (e.g. the 20 MB Pi4 kit, sent without a USB stick; the rest later)
#     --archive  also <out dir>.tar: the whole kit as one file to send
set -euo pipefail
repo="$(cd "$(dirname "$0")/.." && pwd)"
nodes=(NODE-RPI4-01 NODE-RPI5-01 NODE-RPI5-02 PHONE-APP)
archive=0
while [[ $# -gt 0 ]]; do
  case "$1" in
    --nodes) read -ra nodes <<< "$2"; shift 2;;
    --archive) archive=1; shift;;
    -*) echo "unknown option $1" >&2; exit 2;;
    *) break;;
  esac
done
out="${1:-$HOME/Desktop/HomeMind-field-kit}"
cd "$repo"
. deploy/common/payload.sh
if [[ -n "$(git status --porcelain)" ]]; then
  echo "uncommitted changes: commit first (the bundle carries committed code only)" >&2
  git status --short | head -10 >&2
  exit 2
fi
for node in "${nodes[@]}"; do
  echo "== $node"
  payload_verify "deploy/$node/payload"
done

rm -rf "$out"
mkdir -p "$out"
git bundle create "$out/homemind.bundle" HEAD main
tar -cf "$out/payloads.tar" $(for node in "${nodes[@]}"; do printf 'deploy/%s/payload ' "$node"; done)
cat > "$out/site.env" <<'EOF'
# Router 10.10.10.1 (DHCP from .100). Addresses only: never passwords, tokens or Wi-Fi keys.
SSH_USER=root
RPI4_01_HOST=10.10.10.2
RPI5_01_HOST=10.10.10.3
RPI5_02_HOST=10.10.10.4
BROKER_PORT=1884
EOF
cat > "$out/README-설치순서.txt" <<EOF
HomeMind 현장 설치 묶음 ($(git log -1 --format='%h %ad' --date=short))

맥북에서 (터미널, 이 폴더에서):
1) 도구:            brew install openssl@3
2) 묶음 확인:        shasum -a 256 -c KIT_SHA256SUMS
3) 코드 풀기:        git clone homemind.bundle ~/HomeMind
4) 설치 파일 풀기:   tar -xf payloads.tar -C ~/HomeMind && cp site.env ~/HomeMind/deploy/site.env
5) 점검:            cd ~/HomeMind && bash deploy/preflight.sh --ssh      -> PREFLIGHT_PASS
   Pi를 켤 때마다:   bash deploy/sync-time.sh   (Pi에 시계 배터리가 없어 켜면 날짜가 돌아감, 맥북 시간을 넣음)

설치(사용자 승인 후, 순서대로):
6) Pi4 core 0.6.5:  bash deploy/NODE-RPI4-01/install.sh            (운영 DB 백업 후 설치)
7) MQTT 계정:        Pi4에서 사람이 mosquitto_passwd (deploy/NODE-RPI4-01/README.md)
8) Mobile API:       bash deploy/NODE-RPI4-01/install-mobile-api.sh
9) Pi5-02, Pi5-01:   bash deploy/NODE-RPI5-02/install.sh / bash deploy/NODE-RPI5-01/install.sh
10) ESP32·ESP8266:   firmware/include/secrets.h 채우고 bash deploy/CTRL-ESP32-ACT-01/flash.sh /dev/cu.usbserial-…
11) 휴대폰:          deploy/PHONE-APP/payload/homemind-app-release.apk 설치 → 7인치 더보기 → 휴대폰 연결 QR

맥북 유선 주소가 안 잡히면: sudo ipconfig set en7 MANUAL 10.10.10.50 255.255.255.0
EOF
(cd "$out" && for f in homemind.bundle payloads.tar site.env "README-설치순서.txt"; do
   printf '%s  %s\n' "$(hm_sha256 "$f")" "$f"
 done > KIT_SHA256SUMS)
echo "== field kit: $out (payloads: ${nodes[*]})"
ls -la "$out"
if [[ $archive -eq 1 ]]; then
  tar -cf "$out.tar" -C "$(dirname "$out")" "$(basename "$out")"
  echo "== one file: $out.tar ($(du -h "$out.tar" | cut -f1))"
fi
