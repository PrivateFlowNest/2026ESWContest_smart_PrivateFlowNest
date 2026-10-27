param(
    [switch]$Setup,
    [switch]$Full
)

$ErrorActionPreference = "Stop"
$serviceDir = "/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/exaone-edge"

if ($Setup) {
    & wsl.exe -d Ubuntu-22.04 -- bash "$serviceDir/training/setup_wsl.sh"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$mode = if ($Full) { "full" } else { "smoke" }
Write-Host "Starting EXAONE LoRA training mode: $mode"
& wsl.exe -d Ubuntu-22.04 -- bash "$serviceDir/training/run_lora.sh" $mode
exit $LASTEXITCODE
