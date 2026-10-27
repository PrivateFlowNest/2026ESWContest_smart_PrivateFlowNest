#!/usr/bin/env bash
# HomeMind release signing key (ed25519, OpenSSL 3).
#   bash deploy/release-key.sh init     create the private key (~/.homemind, mode 0600) and write the public key to
#                                       deploy/keys/homemind-release-ed25519.pub (commit that file, never the private key)
#   bash deploy/release-key.sh show     fingerprint of the public key in deploy/keys/
# Keep a backup of the private key off this PC; without it payloads cannot be signed (installers refuse unsigned ones).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
. "$here/common/payload.sh"

fingerprint() { openssl pkey -pubin -in "$HM_RELEASE_PUB" -outform DER | sha256sum | cut -c1-16; }

case "${1:-}" in
  init)
    if [[ -f "$HM_RELEASE_KEY" ]]; then
      echo "release key already exists: $HM_RELEASE_KEY (not overwritten)" >&2
    else
      mkdir -p "$(dirname "$HM_RELEASE_KEY")"
      (umask 077 && openssl genpkey -algorithm ed25519 -out "$HM_RELEASE_KEY")
      chmod 600 "$HM_RELEASE_KEY" 2>/dev/null || true
      echo "created $HM_RELEASE_KEY (back it up; never commit it)"
    fi
    mkdir -p "$(dirname "$HM_RELEASE_PUB")"
    openssl pkey -in "$HM_RELEASE_KEY" -pubout -out "$HM_RELEASE_PUB"
    echo "public key: $HM_RELEASE_PUB  sha256/16: $(fingerprint)";;
  show)
    echo "public key: $HM_RELEASE_PUB  sha256/16: $(fingerprint)";;
  *) sed -n '2,6p' "$0"; exit 2;;
esac
