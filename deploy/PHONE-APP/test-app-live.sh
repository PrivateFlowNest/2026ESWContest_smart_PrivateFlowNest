#!/usr/bin/env bash
# The app's API client (Dart, test/live_mobile_api_test.dart) against the PC Mobile API started by pc-mobile-api.sh:
# QR pairing (one-time code), dashboard, LED-LIVING-01 ON/OFF through core + fake ESP8266, 401 without the token.
#   bash deploy/PHONE-APP/pc-mobile-api.sh <PC IP>      (first)
#   bash deploy/PHONE-APP/test-app-live.sh
# The QR payload goes to a temp file that is deleted afterwards; the test token is revoked at the end.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
app="$here/../../team/homemind-application-2026-10-21"
FLUTTER="${FLUTTER:-/c/Users/alexi/dev/flutter/bin/flutter}"
name=hm-phone-mobile-api
api() { MSYS_NO_PATHCONV=1 docker exec "$name" /build/services/homemind-mobile-api/homemind-mobile-api \
          --config /data/pc.json "$@"; }
docker inspect "$name" >/dev/null 2>&1 || { echo "start it first: bash deploy/PHONE-APP/pc-mobile-api.sh <PC IP>" >&2; exit 2; }

qr="$(mktemp)"
revoke_test_tokens() {
  rm -f "$qr"
  api --list-tokens | awk '$NF == "live-test" {print $1}' | while read -r id; do api --revoke-token "$id"; done
}
trap revoke_test_tokens EXIT
api --print-pairing-qr --json --ttl 120 > "$qr"
cd "$app"
HOMEMIND_LIVE_QR_FILE="$(cygpath -w "$qr" 2>/dev/null || echo "$qr")" "$FLUTTER" test test/live_mobile_api_test.dart
