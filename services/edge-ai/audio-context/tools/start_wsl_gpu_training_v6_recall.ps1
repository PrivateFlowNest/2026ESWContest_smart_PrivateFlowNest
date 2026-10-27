param(
    [switch]$Background
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $projectRoot "logs\public-balanced-v6-recall-buffer.wsl.pid"
$stdoutLog = Join-Path $projectRoot "logs\public-balanced-v6-recall-buffer.wsl.stdout.log"
$stderrLog = Join-Path $projectRoot "logs\public-balanced-v6-recall-buffer.wsl.stderr.log"

$drive = $projectRoot.Substring(0, 1).ToLowerInvariant()
$rest = $projectRoot.Substring(2).Replace('\', '/')
$wslScript = "/mnt/$drive$rest/tools/run_wsl_gpu_training_v6_recall.sh"
$wslExe = Join-Path $env:SystemRoot "System32\wsl.exe"

if (-not $Background) {
    & $wslExe -d Ubuntu-22.04 -- bash $wslScript
    exit $LASTEXITCODE
}

$process = Start-Process `
    -FilePath $wslExe `
    -ArgumentList @("-d", "Ubuntu-22.04", "--", "bash", $wslScript) `
    -WorkingDirectory $projectRoot `
    -RedirectStandardOutput $stdoutLog `
    -RedirectStandardError $stderrLog `
    -WindowStyle Hidden `
    -PassThru

[System.IO.File]::WriteAllText($pidFile, "$($process.Id)`n")
$process.Id
