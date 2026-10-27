param(
    [string]$PiAddress = '10.10.10.2'
)

$ErrorActionPreference = 'Stop'
$sshCandidates = @(
    (Join-Path $env:windir 'System32\OpenSSH\ssh.exe'),
    (Join-Path $env:windir 'Sysnative\OpenSSH\ssh.exe')
)
$ssh = $sshCandidates | Where-Object { Test-Path -LiteralPath $_ } | Select-Object -First 1
if (-not $ssh) {
    throw 'Windows OpenSSH client was not found.'
}

$nodeId = 'CTRL-OCTOPUS-01'
$projectRoot = Split-Path -Parent $PSScriptRoot
$knownHosts = Join-Path $projectRoot '.pi4_known_hosts'
$sshOptions = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'StrictHostKeyChecking=yes', '-o', "UserKnownHostsFile=$knownHosts")

& $ssh @sshOptions "root@$PiAddress" "LD_LIBRARY_PATH=/data/share/homemind/lib /data/share/homemind/bin/homemind-simulator --broker-host $PiAddress --broker-port 1883 --node-id $nodeId --profile normal --duration-seconds 6 --interval-ms 1000"
if ($LASTEXITCODE -ne 0) {
    throw 'Fake ESP32 heartbeat test failed.'
}

$nodeCharCodes = (($nodeId.ToCharArray() | ForEach-Object { [int][char]$_ }) -join ',')
$statusSql = "select\(status\)from\(devices\)where\(node_id=char\($nodeCharCodes\)\)\;"
$statusQuery = "sqlite3 /data/share/homemind/db/homemind.db $statusSql"
$before = & $ssh @sshOptions "root@$PiAddress" $statusQuery
if (($before | Select-Object -Last 1).Trim() -ne 'online') {
    throw "Expected online immediately after heartbeat, got: $before"
}
Write-Host 'OFFLINE_TEST before_wait=online'

Start-Sleep -Seconds 17
$after = & $ssh @sshOptions "root@$PiAddress" $statusQuery
if (($after | Select-Object -Last 1).Trim() -ne 'offline') {
    throw "Expected offline after 17 seconds, got: $after"
}
Write-Host 'OFFLINE_TEST after_17_seconds=offline'
Write-Host 'OFFLINE_TEST_PASS'
