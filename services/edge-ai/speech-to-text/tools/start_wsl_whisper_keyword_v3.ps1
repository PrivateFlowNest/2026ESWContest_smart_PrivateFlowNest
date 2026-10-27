$ErrorActionPreference = "Stop"
$serviceDir = "/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/speech-to-text"
Write-Host "Starting HomeMind keyword-augmented Whisper v3 training"
& nvidia-smi --query-gpu=name,memory.used,memory.total,utilization.gpu --format=csv,noheader
& wsl.exe -d Ubuntu-22.04 -- bash "$serviceDir/training/run_keyword_v3.sh"
exit $LASTEXITCODE
