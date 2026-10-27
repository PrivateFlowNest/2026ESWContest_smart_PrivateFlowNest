param(
    [string]$CocoDir = ".\data\figure-coco",
    [string]$Checkpoint = ".\artifacts\training\homemind-yolox-nano-figure-v1\best_ckpt.pth",
    [int]$BatchSize = 128
)

$ErrorActionPreference = "Stop"
$serviceRoot = Split-Path -Parent $PSScriptRoot
$python = Join-Path $serviceRoot ".venv\Scripts\python.exe"
$yoloxRoot = Join-Path $serviceRoot ".cache\torch\hub\Megvii-BaseDetection_YOLOX_main"
$experiment = Join-Path $serviceRoot "training\yolox_nano_figure.py"
function Get-ServicePath([string]$Value) {
    if ([System.IO.Path]::IsPathRooted($Value)) {
        return [System.IO.Path]::GetFullPath($Value)
    }
    return [System.IO.Path]::GetFullPath((Join-Path $serviceRoot $Value))
}
$resolvedCoco = (Resolve-Path -LiteralPath (Get-ServicePath $CocoDir)).Path
$resolvedCheckpoint = (Resolve-Path -LiteralPath (Get-ServicePath $Checkpoint)).Path

$env:HOMEMIND_VISION_COCO_DIR = $resolvedCoco
$env:PYTHONPATH = "$yoloxRoot;$env:PYTHONPATH"

Push-Location $yoloxRoot
try {
    & $python ".\tools\eval.py" `
        -f $experiment `
        -c $resolvedCheckpoint `
        -d 1 `
        -b $BatchSize `
        --fp16 `
        --test
    if ($LASTEXITCODE -ne 0) {
        throw "YOLOX test-set evaluation failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
