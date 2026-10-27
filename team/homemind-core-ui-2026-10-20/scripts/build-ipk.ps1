$ErrorActionPreference = 'Stop'

$projectRoot = Split-Path -Parent $PSScriptRoot
$contractsDir = Join-Path $projectRoot '..\..\contracts\pfn.v1' | Resolve-Path
$ipkDist = Join-Path $projectRoot 'dist-ipk'
$nextIpkDist = Join-Path $projectRoot 'dist-ipk.next'

if (Test-Path -LiteralPath $nextIpkDist) {
    Remove-Item -LiteralPath $nextIpkDist -Recurse -Force
}
New-Item -ItemType Directory -Path $nextIpkDist | Out-Null

docker buildx build `
    --build-context "contracts=$contractsDir" `
    --platform linux/arm64 `
    --target ipk `
    --output "type=local,dest=$nextIpkDist" `
    $projectRoot

if ($LASTEXITCODE -ne 0) {
    Remove-Item -LiteralPath $nextIpkDist -Recurse -Force
    throw "Atlas IPK build failed with exit code $LASTEXITCODE"
}

# Keep earlier IPKs: the installed version is the rollback package.
New-Item -ItemType Directory -Force -Path $ipkDist | Out-Null
Get-ChildItem -LiteralPath $nextIpkDist -Filter '*.ipk' | ForEach-Object {
    $target = Join-Path $ipkDist $_.Name
    if (Test-Path -LiteralPath $target) {
        Remove-Item -LiteralPath $nextIpkDist -Recurse -Force
        throw "Refusing to overwrite existing package: $target (bump package/control Version)"
    }
    Move-Item -LiteralPath $_.FullName -Destination $target
}
Remove-Item -LiteralPath $nextIpkDist -Recurse -Force
Get-ChildItem -LiteralPath $ipkDist -Filter '*.ipk' | Sort-Object Name | ForEach-Object {
    '{0}  {1}' -f (Get-FileHash -Algorithm SHA256 -LiteralPath $_.FullName).Hash.ToLower(), $_.Name
} | Set-Content -Encoding ascii -LiteralPath (Join-Path $ipkDist 'SHA256SUMS')
Write-Host "Atlas IPK: $ipkDist"
