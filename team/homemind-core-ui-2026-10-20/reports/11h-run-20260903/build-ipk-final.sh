#!/bin/sh
set -eu

project_root=/mnt/c/Users/junwo/Documents/Codex/2026-08-26/pi-4-pi4-sql-lite
old_ipk="$project_root/dist-ipk/homemind-core_0.5.0_aarch64.ipk"
new_ipk="$project_root/dist-ipk/homemind-core_0.5.2_aarch64.ipk"
temporary_root=$(mktemp -d)
cleanup() {
  rm -rf -- "$temporary_root"
}
trap cleanup EXIT INT TERM

cd "$temporary_root"
ar x "$old_ipk"
mkdir control data
tar -xzf control.tar.gz -C control
tar -xzf data.tar.gz -C data

install -m 0755 "$project_root/.tmp/arm64-clock-safe/homemind-core" \
  data/data/share/homemind/bin/homemind-core
install -m 0755 "$project_root/.tmp/arm64-clock-safe/homemind-selftest" \
  data/data/share/homemind/bin/homemind-selftest
install -m 0755 "$project_root/.tmp/arm64-clock-safe/homemind-soaktest" \
  data/data/share/homemind/bin/homemind-soaktest
install -m 0755 "$project_root/.tmp/arm64-clock-safe/homemind-simulator" \
  data/data/share/homemind/bin/homemind-simulator
install -m 0644 "$project_root/contracts/sensor-rules.json" \
  data/data/share/homemind/config/sensor-rules.json
install -m 0644 "$project_root/contracts/scenario-rules.json" \
  data/data/share/homemind/config/scenario-rules.json
install -m 0644 "$project_root/contracts/device-registry.json" \
  data/data/share/homemind/config/device-registry.json
install -m 0644 "$project_root/deploy/homemind-core.service" \
  data/data/share/usr/lib/systemd/system/homemind-core.service
install -m 0644 "$project_root/deploy/homemind-ethernet.service" \
  data/data/share/usr/lib/systemd/system/homemind-ethernet.service
install -m 0644 "$project_root/package/homemind-mqtt.service" \
  data/data/share/usr/lib/systemd/system/homemind-mqtt.service
install -m 0644 "$project_root/deploy/homemind-mqtt-ethernet.conf" \
  data/data/share/usr/lib/systemd/system/homemind-mqtt.service.d/10-ethernet.conf
install -m 0755 "$project_root/package/postinst" control/postinst
install -m 0755 "$project_root/package/prerm" control/prerm
install -m 0644 "$project_root/package/conffiles" control/conffiles
sed -i 's/^Version:.*/Version: 0.5.2/' control/control

tar --numeric-owner --owner=0 --group=0 -C control -czf control.tar.gz .
tar --numeric-owner --owner=0 --group=0 -C data -czf data.tar.gz .
printf '2.0\n' >debian-binary
rm -f "$new_ipk"
ar r "$new_ipk" debian-binary control.tar.gz data.tar.gz

ar t "$new_ipk"
tar -xOf control.tar.gz ./control
test "$(tar -xOf control.tar.gz ./conffiles | wc -l)" -eq 3
cmp "$project_root/.tmp/arm64-clock-safe/homemind-core" \
  data/data/share/homemind/bin/homemind-core
cmp "$project_root/.tmp/arm64-clock-safe/homemind-selftest" \
  data/data/share/homemind/bin/homemind-selftest
cmp "$project_root/.tmp/arm64-clock-safe/homemind-soaktest" \
  data/data/share/homemind/bin/homemind-soaktest
cmp "$project_root/.tmp/arm64-clock-safe/homemind-simulator" \
  data/data/share/homemind/bin/homemind-simulator
test -L data/data/share/usr/lib/systemd/system/multi-user.target.wants/homemind-core.service
test -L data/data/share/usr/lib/systemd/system/multi-user.target.wants/homemind-mqtt.service
test -L data/data/share/usr/lib/systemd/system/multi-user.target.wants/homemind-ethernet.service
readelf -h data/data/share/homemind/bin/homemind-core | grep 'Machine:.*AArch64'
sha256sum "$new_ipk"
echo IPK_BUILD_PASS
