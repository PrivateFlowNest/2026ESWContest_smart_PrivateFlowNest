#!/bin/sh
set -eu

database=/data/share/homemind/db/homemind.db
expected_core_sha256=877f37df26bebe442d20552739426f7c044533dd27d356718b3b3978fa704e81

for service in dropbear.socket homemind-ethernet.service homemind-mqtt.service homemind-core.service; do
  active=$(systemctl is-active "$service")
  enabled=$(systemctl is-enabled "$service")
  echo "SERVICE $service active=$active enabled=$enabled"
  test "$active" = active
  test "$enabled" = enabled
done

root_options=$(findmnt -n -o OPTIONS /)
echo "ROOT_OPTIONS $root_options"
case ",$root_options," in
  *,ro,*) ;;
  *) echo "ROOT_FILESYSTEM_NOT_READ_ONLY" >&2; exit 1 ;;
esac

actual_core_sha256=$(sha256sum /data/share/homemind/bin/homemind-core | awk '{print $1}')
echo "CORE_SHA256 $actual_core_sha256"
test "$actual_core_sha256" = "$expected_core_sha256"

integrity=$(sqlite3 "$database" 'PRAGMA integrity_check;')
echo "integrity=$integrity"
test "$integrity" = ok
sqlite3 "$database" "select 'events='||count(*) from mqtt_events; select 'commands='||count(*) from commands; select 'acks='||count(*) from acknowledgements; select 'telemetry='||count(*) from telemetry; select 'heartbeats='||count(*) from heartbeats; select 'automatic_delete='||value from metadata where key='automatic_delete_enabled'; select 'clock_valid='||last_time_valid||',boot_id='||boot_id||',uptime_ms='||last_seen_uptime_ms from boot_sessions order by rowid desc limit 1;"

automatic_delete=$(sqlite3 "$database" "select value from metadata where key='automatic_delete_enabled';")
test "$automatic_delete" = false
echo "POST_REBOOT_CHECK_PASS"
