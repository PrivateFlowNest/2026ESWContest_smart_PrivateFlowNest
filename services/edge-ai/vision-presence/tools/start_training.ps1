param(
    [string]$DatasetRoot = ".\data\figure-dataset",
    [string]$CocoDir = ".\data\figure-coco",
    [int]$BatchSize = 128,
    [int]$Epochs = 150
)

$ErrorActionPreference = "Stop"
$serviceRoot = Split-Path -Parent $PSScriptRoot
$python = Join-Path $serviceRoot ".venv\Scripts\python.exe"
$cli = Join-Path $serviceRoot ".venv\Scripts\homemind-vision.exe"
$yoloxRoot = Join-Path $serviceRoot ".cache\torch\hub\Megvii-BaseDetection_YOLOX_main"
$checkpoint = Join-Path $serviceRoot ".cache\torch\hub\checkpoints\yolox_nano.pth"
$experiment = Join-Path $serviceRoot "training\yolox_nano_figure.py"
$trainingOutput = Join-Path $serviceRoot "artifacts\training"
function Get-ServicePath([string]$Value) {
    if ([System.IO.Path]::IsPathRooted($Value)) {
        return [System.IO.Path]::GetFullPath($Value)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $serviceRoot $Value))
}

foreach ($required in @($python, $cli, $yoloxRoot, $checkpoint, $experiment)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Missing required path: $required"
    }
}

$resolvedDataset = (Resolve-Path -LiteralPath (Get-ServicePath $DatasetRoot)).Path
$resolvedCoco = (Resolve-Path -LiteralPath (Get-ServicePath $CocoDir)).Path

& $cli dataset ready --root $resolvedDataset
if ($LASTEXITCODE -ne 0) {
    throw "Dataset readiness gate failed. Do not train on incomplete or leaked data."
}

foreach ($annotation in @("instances_train2017.json", "instances_val2017.json", "instances_test2017.json")) {
    $path = Join-Path $resolvedCoco "annotations\$annotation"
    if (-not (Test-Path -LiteralPath $path)) {
        throw "Missing COCO annotation: $path"
    }
}

& $python -c "import torch; assert torch.cuda.is_available(), 'CUDA GPU is not available'; print(torch.cuda.get_device_name(0))"
if ($LASTEXITCODE -ne 0) { throw "CUDA check failed." }

$env:HOMEMIND_VISION_COCO_DIR = $resolvedCoco
$env:HOMEMIND_VISION_TRAINING_OUTPUT = $trainingOutput
$env:PYTHONPATH = "$yoloxRoot;$env:PYTHONPATH"

Push-Location $yoloxRoot
try {
    & $python ".\tools\train.py" `
        -f $experiment `
        -d 1 `
        -b $BatchSize `
        --fp16 `
        --cache ram `
        -c $checkpoint `
        max_epoch $Epochs
    if ($LASTEXITCODE -ne 0) {
        throw "YOLOX training failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
