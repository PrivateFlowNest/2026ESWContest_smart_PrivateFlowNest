# Release signing of device payloads (sourced by collect.sh and the install scripts; needs OpenSSL 3 for ed25519).
# A payload's SHA256SUMS is signed with the HomeMind release key into SHA256SUMS.sig. Installers check the signature
# with the public key committed in deploy/keys/ and then every file hash, before anything reaches a device.
#   private key  ${HM_RELEASE_KEY:-~/.homemind/homemind-release-ed25519.pem}   (never in the repository)
#   public key   ${HM_RELEASE_PUB:-deploy/keys/homemind-release-ed25519.pub}
# HM_ALLOW_UNSIGNED=1 lets an installer accept an unsigned payload (PC tests only; it says so loudly).

_hm_deploy_root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
HM_RELEASE_KEY="${HM_RELEASE_KEY:-$HOME/.homemind/homemind-release-ed25519.pem}"
HM_RELEASE_PUB="${HM_RELEASE_PUB:-$_hm_deploy_root/keys/homemind-release-ed25519.pub}"

# Paths for openssl: on Git Bash it is a Windows program, and MSYS_NO_PATHCONV=1 (set by the Docker tests) stops
# the automatic /c/... -> C:/... conversion.
_hm_os_path() { if command -v cygpath >/dev/null 2>&1; then cygpath -m "$1"; else printf '%s' "$1"; fi; }

# The laptop that runs the installers may be Git Bash, Linux or macOS (bash 3.2, BSD tools):
# hm_openssl   OpenSSL 3 (ed25519 -rawin). macOS /usr/bin/openssl is LibreSSL; Homebrew's openssl@3 is keg-only.
hm_openssl() {
  local candidate
  for candidate in "${HM_OPENSSL:-}" openssl /opt/homebrew/opt/openssl@3/bin/openssl /usr/local/opt/openssl@3/bin/openssl; do
    [[ -n "$candidate" ]] && command -v "$candidate" >/dev/null 2>&1 || continue
    if "$candidate" version 2>/dev/null | grep -q '^OpenSSL 3'; then "$candidate" "$@"; return; fi
  done
  echo "OpenSSL 3 not found (macOS: brew install openssl@3)" >&2
  return 127
}
# hm_sha256 FILE / hm_sha256_check LIST: GNU sha256sum, or shasum (macOS)
hm_sha256() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum "$1" | cut -d' ' -f1; else shasum -a 256 "$1" | cut -d' ' -f1; fi
}
hm_sha256_check() {
  if command -v sha256sum >/dev/null 2>&1; then sha256sum -c --quiet "$1"; return; fi
  shasum -a 256 -c "$1" | grep -v ': OK$'
  return "${PIPESTATUS[0]}"
}
# hm_ssh_shared_opts: one SSH login per install run on macOS/Linux (one password prompt, not one per command);
# Git Bash keeps plain ssh. Usage: while read -r o; do ssh_opts+=("$o"); done < <(hm_ssh_shared_opts)
hm_ssh_shared_opts() {
  case "$(uname -s)" in
    Darwin|Linux)
      mkdir -p -m 700 "$HOME/.ssh"
      printf '%s\n' -o ControlMaster=auto -o "ControlPath=$HOME/.ssh/hm-%C" -o ControlPersist=120 ;;
  esac
}
# hm_mosquitto_hash USER < password: one mosquitto.passwd line in the format the device broker (mosquitto 2.0.18)
# reads: $7$ = PBKDF2-HMAC-SHA512, 101 iterations, 12-byte salt. Homebrew's mosquitto_passwd 2.1 writes 1000
# iterations and a 64-byte salt, which the real Pi4 refused ("not authorised", 2026-10-23). The password comes on
# stdin, never as an argument.
hm_mosquitto_hash() {
  local py
  py="$(command -v python3 || command -v python || true)"
  [[ -n "$py" ]] || { echo "python3 is needed to hash MQTT passwords" >&2; return 127; }
  "$py" -c 'import base64, hashlib, os, sys
user = sys.argv[1]
password = sys.stdin.readline().rstrip("\r\n").encode()
salt = os.urandom(12)
digest = hashlib.pbkdf2_hmac("sha512", password, salt, 101)
print("%s:$7$101$%s$%s" % (user, base64.b64encode(salt).decode(), base64.b64encode(digest).decode()))' "$1"
}
# hm_etc_write_cmd PATH: device-side shell command that writes its stdin to PATH (under /etc), remounting / rw only
# for that moment (Atlas keeps / read-only). systemd takes boot units and drop-ins from /etc: a drop-in or enable
# link only in /data/share/usr/lib/systemd/system was not used after a reboot of the real Pi5-02 (2026-10-23).
# Usage: on "$(hm_etc_write_cmd /etc/systemd/system/x.service.d/y.conf)" < file
hm_etc_write_cmd() {
  printf '%s' "ro=0; case \",\$(awk '\$2 == \"/\" { print \$4; exit }' /proc/mounts),\" in *,ro,*) mount -o remount,rw / && ro=1;; esac; \
( set -e; mkdir -p \"\$(dirname $1)\"; cat > $1.new; chmod 0644 $1.new; mv $1.new $1 ); rc=\$?; sync; \
if [ \$ro -eq 1 ]; then mount -o remount,ro /; fi; exit \$rc"
}
# hm_enable_cmd UNIT...: device-side command that enables units for boot with / rw for the moment (the 0.1.0
# packages' postinst enabled after remounting read-only, which failed silently), then prints is-enabled per unit.
hm_enable_cmd() {
  printf '%s' "ro=0; case \",\$(awk '\$2 == \"/\" { print \$4; exit }' /proc/mounts),\" in *,ro,*) mount -o remount,rw / && ro=1;; esac; \
systemctl daemon-reload; systemctl enable $* 2>&1 | grep -v '^Created symlink' | sed 's/^/  /'; sync; \
if [ \$ro -eq 1 ]; then mount -o remount,ro /; fi; for u in $*; do printf '  %s: ' \$u; systemctl is-enabled \$u 2>/dev/null || true; done"
}
# hm_scp: OpenSSH 9+ copies over SFTP; a device without an SFTP server needs the legacy protocol (-O)
hm_scp() {
  scp "$@" && return
  echo "  scp failed; retrying with the legacy scp protocol (-O)" >&2
  scp -O "$@"
}

# payload_sign <dir>: sign <dir>/SHA256SUMS when the private key is here; otherwise leave it unsigned (warning).
payload_sign() {
  local dir=$1
  rm -f "$dir/SHA256SUMS.sig"
  if [[ ! -f "$HM_RELEASE_KEY" ]]; then
    echo "  WARNING: no release key ($HM_RELEASE_KEY): payload left UNSIGNED, installers will refuse it" >&2
    echo "           create one with: bash deploy/release-key.sh init" >&2
    return 0
  fi
  hm_openssl pkeyutl -sign -inkey "$(_hm_os_path "$HM_RELEASE_KEY")" -rawin -in "$(_hm_os_path "$dir/SHA256SUMS")" \
    -out "$(_hm_os_path "$dir/SHA256SUMS.sig")"
  echo "  signed: $dir/SHA256SUMS.sig"
}

# payload_verify <dir>: signature (unless HM_ALLOW_UNSIGNED=1 and there is none), then every hash. Returns non-zero
# with a message on any problem.
payload_verify() {
  local dir=$1 result
  [[ -f "$dir/SHA256SUMS" ]] || { echo "no SHA256SUMS in $dir: run bash deploy/collect.sh first" >&2; return 2; }
  if [[ -f "$dir/SHA256SUMS.sig" ]]; then
    [[ -f "$HM_RELEASE_PUB" ]] || { echo "release public key missing: $HM_RELEASE_PUB" >&2; return 1; }
    if ! result="$(hm_openssl pkeyutl -verify -pubin -inkey "$(_hm_os_path "$HM_RELEASE_PUB")" -rawin \
        -in "$(_hm_os_path "$dir/SHA256SUMS")" -sigfile "$(_hm_os_path "$dir/SHA256SUMS.sig")" 2>&1)"; then
      if grep -q "Signature Verification Failure" <<< "$result"; then
        echo "payload signature INVALID ($dir): not made with the HomeMind release key" >&2
      else
        echo "payload signature could not be checked ($dir): $(head -1 <<< "$result")" >&2
      fi
      return 1
    fi
    echo "  payload signature OK ($(basename "$HM_RELEASE_PUB"))"
  elif [[ "${HM_ALLOW_UNSIGNED:-0}" == 1 ]]; then
    echo "  WARNING: unsigned payload accepted (HM_ALLOW_UNSIGNED=1, tests only)" >&2
  else
    echo "payload is not signed ($dir): collect it on the PC that holds the release key" >&2
    return 1
  fi
  (cd "$dir" && hm_sha256_check SHA256SUMS) || { echo "payload hash mismatch ($dir)" >&2; return 1; }
}
