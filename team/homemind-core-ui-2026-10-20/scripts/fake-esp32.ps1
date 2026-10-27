param(
    [ValidateSet('normal', 'out-of-range', 'wrong-type', 'unknown', 'mixed')]
    [string]$Profile = 'normal',
    [ValidateRange(1, 86400)]
    [int]$DurationSeconds = 30,
    [string]$NodeId = 'CTRL-ESP32-ACT-01',
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
$projectRoot = Split-Path -Parent $PSScriptRoot
$knownHosts = Join-Path $projectRoot '.pi4_known_hosts'
$sshOptions = @('-o', 'BatchMode=yes', '-o', 'ConnectTimeout=5', '-o', 'StrictHostKeyChecking=yes', '-o', "UserKnownHostsFile=$knownHosts")
& $ssh @sshOptions "root@$PiAddress" "LD_LIBRARY_PATH=/data/share/homemind/lib /data/share/homemind/bin/homemind-simulator --broker-host $PiAddress --broker-port 1883 --node-id $NodeId --profile $Profile --duration-seconds $DurationSeconds --interval-ms 1000"
if ($LASTEXITCODE -ne 0) {
    throw "Fake device simulator failed with exit code $LASTEXITCODE"
}
