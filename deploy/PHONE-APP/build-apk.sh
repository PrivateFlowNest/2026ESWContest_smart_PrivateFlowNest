#!/usr/bin/env bash
# Build the HomeMind phone app (Flutter, Android) and put the APK into deploy/PHONE-APP/payload/ with its SHA-256.
#   bash deploy/PHONE-APP/build-apk.sh
# Paths (override with environment variables): FLUTTER (Flutter SDK bin/flutter), ANDROID_HOME (Android SDK),
# JAVA_HOME (JDK 17). Nothing is written to the Flutter or Android Studio settings; the variables only apply here.
# The release APK is signed with the Flutter debug key (sideload for the demo phone, not for a store).
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
app="$here/../../team/homemind-application-2026-10-21"
FLUTTER="${FLUTTER:-/c/Users/alexi/dev/flutter/bin/flutter}"
export ANDROID_HOME="${ANDROID_HOME:-C:\\Android\\sdk}" ANDROID_SDK_ROOT="${ANDROID_SDK_ROOT:-${ANDROID_HOME:-C:\\Android\\sdk}}"
export JAVA_HOME="${JAVA_HOME:-C:\\Android\\jdk\\jdk-17.0.20.1+1}"
[[ -x "$FLUTTER" || -f "$FLUTTER.bat" ]] || { echo "Flutter not found: $FLUTTER (set FLUTTER=...)" >&2; exit 2; }

(cd "$app" && "$FLUTTER" test && "$FLUTTER" build apk --release)
apk="$app/build/app/outputs/flutter-apk/app-release.apk"
out="$here/payload"
rm -rf "$out"; mkdir -p "$out"
cp "$apk" "$out/homemind-app-release.apk"
(cd "$out" && sha256sum homemind-app-release.apk > SHA256SUMS && cat SHA256SUMS)
. "$here/../common/payload.sh"
payload_sign "$out"  # release key (deploy/release-key.sh); unsigned when the key is not on this PC
echo "  $(du -h "$out/homemind-app-release.apk" | cut -f1)  $out/homemind-app-release.apk"
