$ErrorActionPreference = "Stop"

$serviceRoot = Split-Path -Parent $PSScriptRoot
$exe = Join-Path $serviceRoot ".venv\Scripts\homemind-stt.exe"
$labels = "E:\HomeMind-Datasets\raw\aihub\71405_noisy_command\archives\labels\135.명령어_인식을_위한_소음_환경_데이터\01-1.정식개방데이터\Training\02.라벨링데이터\TL_01.다화자_소음_환경_003.가정환경_02.근거리.zip"
$audio = "E:\HomeMind-Datasets\raw\aihub\71405_noisy_command\extracted\training\TS_01.다화자_소음_환경_003.가정환경_02.근거리"
$output = Join-Path $serviceRoot "artifacts\manifests\aihub-71405-home-near-training-iot-N.jsonl"

if (-not (Test-Path -LiteralPath $audio)) {
    throw "Training audio is not extracted: $audio"
}

& $exe manifest-aihub `
    --labels $labels `
    --audio-root $audio `
    --audio-variant N `
    --split-name training `
    --domains IoT `
    --limit 0 `
    --output $output
exit $LASTEXITCODE
