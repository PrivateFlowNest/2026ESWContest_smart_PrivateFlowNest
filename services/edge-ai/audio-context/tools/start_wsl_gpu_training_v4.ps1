param(
    [switch]$Background
)

$ErrorActionPreference = "Stop"

$projectRoot = Split-Path -Parent $PSScriptRoot
$pidFile = Join-Path $projectRoot "logs\public-balanced-v4-temporal.wsl.pid"
$stdoutLog = Join-Path $projectRoot "logs\public-balanced-v4-temporal.wsl.stdout.log"
$stderrLog = Join-Path $projectRoot "logs\public-balanced-v4-temporal.wsl.stderr.log"

if (Test-Path -LiteralPath $pidFile) {
    $existingPid = (Get-Content -LiteralPath $pidFile -Raw).Trim()
    if ($existingPid -match '^\d+$') {
        $existing = Get-Process -Id ([int]$existingPid) -ErrorAction SilentlyContinue
        if ($null -ne $existing -and $existing.ProcessName -eq "wsl") {
            $existing.Id
            exit 0
        }
    }
}

$drive = $projectRoot.Substring(0, 1).ToLowerInvariant()
$rest = $projectRoot.Substring(2).Replace('\', '/')
$wslScript = "/mnt/$drive$rest/tools/run_wsl_gpu_training_v4.sh"
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
