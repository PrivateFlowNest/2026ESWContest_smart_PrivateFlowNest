#!/usr/bin/env bash
# The 7-inch UI's phone-connect QR, end to end on the PC: the UI's MQTT data source asks for a pairing offer
# (pfn/v1/mobile/pairing/request), the C++ Mobile API answers with a one-time code (pfn/v1/mobile/pairing/offer),
# then a phone-style claim with that QR gets a token; the same QR is refused a second time.
#   bash deploy/PHONE-APP/pc-mobile-api.sh <PC IP>      (first; PC demo broker on 1883)
#   bash deploy/PHONE-APP/test-ui-qr-live.sh
# The test token is revoked at the end. SIMULATION: PC broker, not the device ACL.
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
ui="$here/../../team/homemind-core-ui-2026-10-20/homemind_flutter_test"
FLUTTER="${FLUTTER:-/c/Users/alexi/dev/flutter/bin/flutter}"
name=hm-phone-mobile-api
api() { MSYS_NO_PATHCONV=1 docker exec "$name" /build/services/homemind-mobile-api/homemind-mobile-api \
          --config /data/pc.json "$@"; }
docker inspect "$name" >/dev/null 2>&1 || { echo "start it first: bash deploy/PHONE-APP/pc-mobile-api.sh <PC IP>" >&2; exit 2; }
revoke_test_tokens() {
  api --list-tokens | awk '$NF ~ /^ui-qr-live-test/ {print $1}' | while read -r id; do api --revoke-token "$id"; done
}
trap revoke_test_tokens EXIT
cd "$ui"
"$FLUTTER" test test/phone_pairing_live_test.dart --dart-define=RUN_BROKER_LIVE=true --dart-define=HOMEMIND_PI4_HOST=127.0.0.1
