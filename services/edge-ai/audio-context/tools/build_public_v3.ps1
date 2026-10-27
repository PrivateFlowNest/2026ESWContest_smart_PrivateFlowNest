$ErrorActionPreference = "Stop"

$project = (Resolve-Path (Join-Path $PSScriptRoot "..")).Path
$cli = Join-Path $project ".venv\Scripts\homemind-audio.exe"
$tauRoot = "E:\HomeMind-Datasets\raw\zenodo\10378145_tau_sound_privacy"
$realisedRoot = "E:\HomeMind-Datasets\raw\zenodo\6488321_realised"

& $cli dataset build-tau-manifest `
    --root (Join-Path $tauRoot "extracted") `
    --metadata (Join-Path $tauRoot "metadata.csv") `
    --reference-manifest (Join-Path $project "data\fsd50k.csv") `
    --output (Join-Path $project "data\tau-doorbell.csv") `
    --max-per-label 0

& $cli dataset build-realised-manifest `
    --root (Join-Path $realisedRoot "extracted") `
    --output (Join-Path $project "data\realised-hard-negatives.csv") `
    --max-per-label 0

& $cli manifest merge `
    --input (Join-Path $project "data\public-full.csv") `
    --input (Join-Path $project "data\tau-doorbell.csv") `
    --input (Join-Path $project "data\realised-hard-negatives.csv") `
    --output (Join-Path $project "data\public-full-v3.csv")

& $cli manifest balance `
    --input (Join-Path $project "data\public-full-v3.csv") `
    --output (Join-Path $project "data\public-balanced-v3.csv") `
    --max-train-per-label 4000 `
    --max-validation-per-label 1000 `
    --max-test-per-label 1000 `
    --max-segments-per-group 4 `
    --balance-datasets `
    --seed 42

& $cli readiness `
    --manifest (Join-Path $project "data\public-balanced-v3.csv") `
    --report (Join-Path $project "data\public-balanced-v3.readiness.json")
