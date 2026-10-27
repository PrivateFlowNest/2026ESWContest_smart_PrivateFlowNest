$ErrorActionPreference = "Stop"
$serviceDir = "/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/speech-to-text"
Write-Host "Starting conservative HomeMind Whisper v4 training"
& nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader
& wsl.exe -d Ubuntu-22.04 -- bash "$serviceDir/training/run_focused_v4.sh"
exit $LASTEXITCODE
