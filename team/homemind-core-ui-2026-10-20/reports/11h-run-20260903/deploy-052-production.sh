#!/bin/sh
set -eu

candidate=/data/share/homemind/testing-11h/bin/homemind-core.052-final
target=/data/share/homemind/bin/homemind-core
next_target=/data/share/homemind/bin/homemind-core.next-052
rollback=/data/share/homemind/rollback-11h-20260903/homemind-core.pre-052
database=/data/share/homemind/db/homemind.db
expected_sha256=877f37df26bebe442d20552739426f7c044533dd27d356718b3b3978fa704e81

actual_candidate=$(sha256sum "$candidate" | cut -d' ' -f1)
test "$actual_candidate" = "$expected_sha256"
test "$(sqlite3 "$database" 'pragma integrity_check;')" = ok
test "$(sqlite3 "$database" "select value from metadata where key='automatic_delete_enabled';")" = false
test ! -e "$rollback"
if [ "${1:-}" = "--check-only" ]; then
  echo "DEPLOY_052_PRECHECK_PASS candidate_sha256=$actual_candidate"
  exit 0
fi

mkdir -p /data/share/homemind/rollback-11h-20260903
cp -p "$target" "$rollback"
cp "$candidate" "$next_target"
chmod 0755 "$next_target"
chown network:network "$next_target"
sync

systemctl stop homemind-core.service
mv -f "$next_target" "$target"
if systemctl start homemind-core.service && \
   test "$(systemctl is-active homemind-core.service)" = active && \
   test "$(sha256sum "$target" | cut -d' ' -f1)" = "$expected_sha256" && \
   test "$(sqlite3 "$database" 'pragma integrity_check;')" = ok; then
  echo "DEPLOY_052_PASS"
  exit 0
fi

echo "DEPLOY_052_FAILED_ROLLING_BACK" >&2
systemctl stop homemind-core.service 2>/dev/null || true
cp -p "$rollback" "$next_target"
mv -f "$next_target" "$target"
systemctl start homemind-core.service
echo "ROLLBACK_RESTORED sha256=$(sha256sum "$target" | cut -d' ' -f1)" >&2
exit 1
