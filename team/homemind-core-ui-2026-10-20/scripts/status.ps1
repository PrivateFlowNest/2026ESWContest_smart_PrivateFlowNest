$ErrorActionPreference = 'Stop'

$sshCandidates = @(
    (Join-Path $env:windir 'System32\OpenSSH\ssh.exe'),
    (Join-Path $env:windir 'Sysnative\OpenSSH\ssh.exe')
)
$ssh = $sshCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $ssh) {
    throw 'Windows OpenSSH client was not found.'
}
$projectRoot = Split-Path -Parent $PSScriptRoot
$knownHosts = Join-Path $projectRoot '.pi4_known_hosts'
$sshOptions = @(
    '-o', 'BatchMode=yes',
    '-o', 'ConnectTimeout=5',
    '-o', 'StrictHostKeyChecking=yes',
    '-o', "UserKnownHostsFile=$knownHosts"
)

$remoteScript = @'
echo '=== HomeMind services ==='
systemctl show homemind-ethernet homemind-mqtt homemind-core -p Id -p ActiveState -p SubState -p NRestarts
echo '=== Installed core ==='
sha256sum /data/share/homemind/bin/homemind-core
sqlite3 /data/share/homemind/db/homemind.db "select 'reported_version='||coalesce(version,'unknown') from heartbeats where node_id='NODE-RPI4-01' order by rowid desc limit 1;"
echo '=== Database ==='
sqlite3 /data/share/homemind/db/homemind.db "select 'events',count(*) from mqtt_events union all select 'commands',count(*) from commands union all select 'acks',count(*) from acknowledgements union all select 'telemetry',count(*) from telemetry union all select 'heartbeats',count(*) from heartbeats; pragma integrity_check;"
echo '=== Storage monitor ==='
sqlite3 /data/share/homemind/db/homemind.db "select 'automatic_delete='||value from metadata where key='automatic_delete_enabled'; select printf('database=%.2f MiB',database_bytes/1048576.0) from storage_status where singleton_id=1; select printf('storage_used=%.1f%% / warning_at=%d%%',filesystem_used_percent,warning_threshold_percent) from storage_status where singleton_id=1; select case warning_active when 1 then 'WARNING: storage usage is at or above 70%' else 'OK: storage usage is below 70%' end from storage_status where singleton_id=1;"
echo '=== Clock safety ==='
sqlite3 /data/share/homemind/db/homemind.db "select 'boot_id='||boot_id,case last_time_valid when 1 then 'time=VALID' else 'time=UNVERIFIED (data is still saved)' end,printf('uptime=%.1f sec',last_seen_uptime_ms/1000.0) from boot_sessions order by rowid desc limit 1; select 'new_events_with_unverified_time='||count(*) from mqtt_events where first_receiver_time_valid=0;"
'@
$remoteScript | & $ssh @sshOptions root@10.10.10.2 sh -s
if ($LASTEXITCODE -ne 0) {
    throw "Pi4 status check failed with exit code $LASTEXITCODE"
}
