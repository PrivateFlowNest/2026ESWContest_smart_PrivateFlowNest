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

& $ssh @sshOptions root@10.10.10.2 'LD_LIBRARY_PATH=/data/share/homemind/lib /data/share/homemind/bin/homemind-selftest --broker-host 10.10.10.2 --broker-port 1883 --database /data/share/homemind/db/homemind.db --node-id CTRL-ESP32-ACT-01'
if ($LASTEXITCODE -ne 0) {
    throw "Pi4 self-test failed with exit code $LASTEXITCODE"
}
