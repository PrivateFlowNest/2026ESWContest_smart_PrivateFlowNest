$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$contractsDir = Join-Path $projectRoot '..\..\contracts\pfn.v1' | Resolve-Path
$dist = Join-Path $projectRoot 'dist'
$nextDist = Join-Path $projectRoot 'dist.next'

if (Test-Path -LiteralPath $nextDist) {
    Remove-Item -LiteralPath $nextDist -Recurse -Force
}
New-Item -ItemType Directory -Path $nextDist | Out-Null

docker buildx build `
    --build-context "contracts=$contractsDir" `
    --platform linux/arm64 `
    --target artifact `
    --output "type=local,dest=$nextDist" `
    $projectRoot

$buildExitCode = $LASTEXITCODE
if ($buildExitCode -ne 0) {
    Remove-Item -LiteralPath $nextDist -Recurse -Force
    throw "Pi4 ARM64 build failed with exit code $buildExitCode"
}

if (Test-Path -LiteralPath $dist) {
    Remove-Item -LiteralPath $dist -Recurse -Force
}
Move-Item -LiteralPath $nextDist -Destination $dist

Write-Host "Pi4 artifacts: $dist"
