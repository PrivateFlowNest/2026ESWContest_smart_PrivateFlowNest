$ErrorActionPreference = "Stop"

$serviceRoot = Split-Path -Parent $PSScriptRoot
$python = Join-Path $serviceRoot ".venv\Scripts\python.exe"
& $python (Join-Path $serviceRoot "training\build_targeted_manifest.py") `
    --input (Join-Path $serviceRoot "artifacts\manifests\aihub-71405-home-near-training-iot-N.jsonl") `
    --output (Join-Path $serviceRoot "artifacts\manifests\aihub-71405-home-near-training-homemind-weighted-v2.jsonl")
exit $LASTEXITCODE
