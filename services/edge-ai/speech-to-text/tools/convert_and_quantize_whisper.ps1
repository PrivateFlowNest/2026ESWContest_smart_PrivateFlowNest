param(
    [string]$TrainingRun = "whisper-small-homemind-lora-v1",
    [ValidateSet("q4_0", "q5_0", "q5_k", "q8_0")]
    [string]$Quantization = "q5_0"
)

$ErrorActionPreference = "Stop"
$serviceRoot = Split-Path -Parent $PSScriptRoot
$runRoot = Join-Path $serviceRoot "artifacts\training\$TrainingRun"
$merged = Join-Path $runRoot "merged"
$ggml = Join-Path $runRoot "ggml-model.bin"
$quantized = Join-Path $runRoot "ggml-$TrainingRun-$Quantization.bin"
$converter = Join-Path $serviceRoot "models\whisper.cpp-b4938-source\whisper.cpp-b4938\models\convert-h5-to-ggml.py"
$openAiWhisper = Join-Path $serviceRoot "models\openai-whisper-main-source\whisper-main"
$quantizer = Join-Path $serviceRoot "models\whisper-bin-x64-b4938\Release\whisper-quantize.exe"

foreach ($required in @($merged, $converter, $openAiWhisper, $quantizer)) {
    if (-not (Test-Path -LiteralPath $required)) {
        throw "Required conversion input does not exist: $required"
    }
}

function Convert-ToWslPath([string]$Path) {
    $resolved = (Resolve-Path -LiteralPath $Path).Path
    $drive = $resolved.Substring(0, 1).ToLowerInvariant()
    $rest = $resolved.Substring(2).Replace('\', '/')
    return "/mnt/$drive$rest"
}

$wslMerged = Convert-ToWslPath $merged
$wslConverter = Convert-ToWslPath $converter
$wslOpenAi = Convert-ToWslPath $openAiWhisper
$wslOutput = Convert-ToWslPath $runRoot
$command = "source `"`$HOME/venvs/homemind-stt/bin/activate`" && python `"$wslConverter`" `"$wslMerged`" `"$wslOpenAi`" `"$wslOutput`""
& wsl.exe -d Ubuntu-22.04 -- bash -lc $command
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

if (-not (Test-Path -LiteralPath $ggml)) {
    throw "GGML conversion output was not created: $ggml"
}
& $quantizer $ggml $quantized $Quantization
if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }

$file = Get-Item -LiteralPath $quantized
$hash = Get-FileHash -LiteralPath $quantized -Algorithm SHA256
[PSCustomObject]@{
    Model = $file.FullName
    Bytes = $file.Length
    SHA256 = $hash.Hash.ToLowerInvariant()
    Quantization = $Quantization
}
