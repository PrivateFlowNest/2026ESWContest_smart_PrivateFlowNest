param(
    [switch]$Setup,
    [switch]$Smoke
)

$ErrorActionPreference = "Stop"
$serviceDir = "/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/speech-to-text"

if ($Setup) {
    & wsl.exe -d Ubuntu-22.04 -- bash "$serviceDir/training/setup_wsl.sh"
    if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
}

$mode = if ($Smoke) { "smoke" } else { "full" }
Write-Host "Starting HomeMind Whisper small LoRA training mode: $mode"
& nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader
& wsl.exe -d Ubuntu-22.04 -- bash "$serviceDir/training/run_lora.sh" $mode
exit $LASTEXITCODE
