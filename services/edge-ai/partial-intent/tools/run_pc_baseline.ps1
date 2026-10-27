[CmdletBinding()]
param(
    [int]$BenchmarkRuns = 1000
)

$ErrorActionPreference = "Stop"
$ServiceRoot = Split-Path -Parent $PSScriptRoot
$LocalPython = Join-Path $ServiceRoot ".venv\Scripts\python.exe"
$SharedPython = Join-Path (Split-Path -Parent $ServiceRoot) "vision-presence\.venv\Scripts\python.exe"
$PythonExe = $null

if (Test-Path -LiteralPath $LocalPython) {
    & $LocalPython -c "import onnxruntime" 2>$null
    if ($LASTEXITCODE -eq 0) {
        $PythonExe = $LocalPython
    }
}

if (-not $PythonExe -and (Test-Path -LiteralPath $SharedPython)) {
    Write-Host "Reusing the existing vision-presence Python environment."
    $PythonExe = $SharedPython
}

if (-not $PythonExe) {
    throw "A Python environment with onnxruntime is required. Install this project's dependencies first."
}

$PreviousPythonPath = $env:PYTHONPATH
$env:PYTHONPATH = Join-Path $ServiceRoot "src"
try {
    & $PythonExe -m homemind_partial_intent.cli dataset
    & $PythonExe -m homemind_partial_intent.cli train
    & $PythonExe -m homemind_partial_intent.cli evaluate
    & $PythonExe -m homemind_partial_intent.cli evaluate `
        --input-file (Join-Path $ServiceRoot "data\acceptance-korean-v3-final.jsonl") `
        --report (Join-Path $ServiceRoot "artifacts\metrics-final-pc-v3.json")
    & $PythonExe -m homemind_partial_intent.cli benchmark --runs $BenchmarkRuns
    & $PythonExe -m pytest -q -p no:cacheprovider
}
finally {
    $env:PYTHONPATH = $PreviousPythonPath
}
