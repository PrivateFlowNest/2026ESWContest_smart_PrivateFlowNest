#!/usr/bin/env bash
# Phone test WITHOUT the Pi 4: run the C++ homemind-mobile-api (the same code as the NODE-RPI4-01 package, x86 build)
# on this PC in Docker, connected to the PC demo broker + core + fake ESP, and print a pairing QR for the phone.
#   bash deploy/PHONE-APP/pc-mobile-api.sh <PC Wi-Fi/LAN IP>     start (port 8000) and print the pairing QR
#   bash deploy/PHONE-APP/pc-mobile-api.sh <PC IP> --https       same over HTTPS (self-signed certificate, pinned via QR)
#   bash deploy/PHONE-APP/pc-mobile-api.sh --qr                  print a new pairing QR (one-time, 10 min)
#   bash deploy/PHONE-APP/pc-mobile-api.sh --stop                stop the API (paired tokens stay in the volume)
# Needs: docker compose -f demo/pc/docker-compose.yml --profile fake-esp up -d  (broker on 1883, anonymous).
# The phone and the PC must be on the same network. SIMULATION: PC core + fake ESP, not the real devices.
# Pairing secret and token hashes live in the Docker volume hm-phone-pc-data (not in the repository).
set -euo pipefail
repo="$(cd "$(dirname "$0")/../.." && pwd)"
name=hm-phone-mobile-api
image="${HM_NATIVE_IMAGE:-homemind-native-dev:24.04}"
export MSYS_NO_PATHCONV=1
host_path() { if command -v cygpath >/dev/null; then cygpath -m "$1"; else echo "$1"; fi; }
qr() { docker exec "$name" /build/services/homemind-mobile-api/homemind-mobile-api --config /data/pc.json \
         --print-pairing-qr; }

case "${1:-}" in
  --stop) docker rm -f "$name" >/dev/null 2>&1 && echo "stopped $name"; exit 0;;
  --qr) qr; exit 0;;
  ""|-h|--help) sed -n '2,9p' "$0"; exit 2;;
esac
ip="$1"
scheme=http; tls=false
if [[ "${2:-}" == "--https" ]]; then scheme=https; tls=true; fi
[[ "$ip" =~ ^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]+$ ]] || { echo "give the PC's IPv4 address on the phone's network" >&2; exit 2; }
(exec 3<>/dev/tcp/127.0.0.1/1883) 2>/dev/null || {
  echo "PC demo broker not running: docker compose -f demo/pc/docker-compose.yml --profile fake-esp up -d" >&2; exit 2; }

echo "== build homemind-mobile-api (x86, Release)"
docker run --rm -v "$(host_path "$repo"):/w" -v hm-native-build-phone:/build "$image" bash -c \
  "cmake -S /w/native -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release -DHM_BUILD_TESTS=OFF >/dev/null && \
   cmake --build /build --target homemind-mobile-api | tail -2"

echo "== start on 0.0.0.0:8000 (base URL $scheme://$ip:8000)"
docker rm -f "$name" >/dev/null 2>&1 || true
config="{\"broker\": {\"host\": \"host.docker.internal\", \"port\": 1883}, \"contracts_dir\": \"/w/contracts\",
  \"device_registry\": \"/w/contracts/pfn.v1/device-registry.json\", \"mqtt_client_id\": \"hm-phone-mobile-api\",
  \"listen_host\": \"0.0.0.0\", \"listen_port\": 8000, \"data_dir\": \"/data/mobile-api\",
  \"public_base_url\": \"$scheme://$ip:8000\", \"tls_enabled\": $tls, \"device_name\": \"HomeMind PC 시험 (NODE-RPI4-01 대역)\"}"
docker run -d --name "$name" -p 8000:8000 -v "$(host_path "$repo"):/w:ro" -v hm-native-build-phone:/build:ro \
  -v hm-phone-pc-data:/data "$image" bash -c "printf '%s' '$config' > /data/pc.json && \
  exec /build/services/homemind-mobile-api/homemind-mobile-api --config /data/pc.json" >/dev/null
for _ in $(seq 1 30); do
  health="$(curl -sk -m 2 "$scheme://127.0.0.1:8000/api/v1/health" || true)"  # -k: readiness only (no data)
  [[ "$health" == *'"mqtt_connected":true'* ]] && break
  sleep 1
done
echo "   health: ${health:-no answer}"
[[ "$health" == *'"mqtt_connected":true'* ]] || { docker logs --tail 20 "$name"; exit 1; }
curl -sk -m 2 "$scheme://$ip:8000/api/v1/health" >/dev/null \
  && echo "   reachable at $scheme://$ip:8000 from this PC" \
  || echo "   WARNING: $scheme://$ip:8000 does not answer from this PC (wrong IP?)"
echo "== pairing QR (scan in the app: 기기 연결 → QR 스캔)"
qr
echo "If the phone cannot connect: same Wi-Fi? Windows firewall must allow inbound TCP 8000 (a person does that)."
