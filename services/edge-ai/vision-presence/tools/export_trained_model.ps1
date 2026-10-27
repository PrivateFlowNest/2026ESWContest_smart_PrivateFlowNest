param(
    [string]$Checkpoint = ".\artifacts\training\homemind-yolox-nano-figure-v1\best_ckpt.pth",
    [string]$Output = ".\artifacts\homemind-yolox-nano-figure-v1.onnx"
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
$resolvedCheckpoint = (Resolve-Path -LiteralPath (Get-ServicePath $Checkpoint)).Path
$outputPath = Get-ServicePath $Output
$cocoDir = [System.IO.Path]::GetFullPath((Join-Path $serviceRoot ".\data\figure-coco"))
$exporter = Join-Path $serviceRoot "tools\export_custom_model.py"

$env:HOMEMIND_VISION_COCO_DIR = $cocoDir
$env:PYTHONPATH = "$yoloxRoot;$env:PYTHONPATH"

Push-Location $yoloxRoot
try {
    & $python $exporter `
        --experiment $experiment `
        --checkpoint $resolvedCheckpoint `
        --output $outputPath
    if ($LASTEXITCODE -ne 0) {
        throw "ONNX export failed with exit code $LASTEXITCODE."
    }
}
finally {
    Pop-Location
}
