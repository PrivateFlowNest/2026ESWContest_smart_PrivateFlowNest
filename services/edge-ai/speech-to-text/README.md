# HomeMind Korean Speech-to-Text

HomeMind의 `MIC-INDOOR-01` 입력을 라즈베리파이 5 안에서만 한국어 텍스트로 바꾸는
서비스입니다. 원본 음성과 전사 텍스트는 Pi 4나 ESP로 보내지 않습니다. Pi 4에는
EXAONE과 결정론적 검증을 통과한 Task 제안만 전달합니다.

## 확정한 실행 구조

1. VAD가 한 발화를 구분합니다.
2. Vosk small Korean이 발화 중간에 빠른 partial transcript를 냅니다.
3. 기존 partial-intent ONNX가 partial transcript를 읽되 `execution_allowed=false`인
   준비 힌트만 만듭니다.
4. 발화가 끝나면 Whisper small이 final transcript를 만듭니다.
5. final transcript만 EXAONE 1.2B의 안전 가드와 Task 제안 입력으로 사용합니다.
6. Pi 4 정책 엔진이 최종 허용 여부와 실제 ID 라우팅을 결정합니다.

Vosk의 최종 문장은 사용하지 않습니다. 실제 AI Hub 소음 음성에서 정확도가 낮았기
때문입니다. 두 단계의 모든 ASR 이벤트는 `scope=PI5_LOCAL_ONLY`이며 계약은
`contracts/speech-transcript.schema.json`입니다.

## PC 후보 비교 결과

AI Hub 71405 검증 분할의 `가정환경/근거리/-N` 원본 100개를 고정해 비교했습니다.

| 후보 | CER | RTF | HomeMind 핵심어 재현율 | 판정 |
|---|---:|---:|---:|---|
| Whisper small | 23.33% | 0.443 | 66.34% | 파인튜닝 기준 모델 |
| Whisper small + HomeMind 용어 프롬프트 | 22.72% | 0.840 | 63.37% | 프롬프트만으로는 부족 |
| Whisper base | 31.53% | 0.224 | 56.44% | 메모리 절약 대안 |
| Korean Zipformer INT8 | 40.35% | 0.026 | 39.60% | 속도 기준선 |
| Vosk small Korean | 71.96% | 0.641 | 20.79% | partial 힌트 전용 |

목표는 CER 25% 이하, RTF 1.0 이하, 핵심어 재현율 90% 이상입니다. Whisper small은
앞의 두 항목만 통과했으므로 아직 배포 모델이 아닙니다. 상세 집계에는 원문과 오디오
경로를 넣지 않았습니다.

원본 15,281개로 학습한 v1 Q5_0은 CER 12.61%, RTF 0.480, 핵심어 재현율
81.19%였습니다. 취약 기기어 3,032개 가중 행으로 추가 학습한 v2 Q5_0은 CER
11.70%, RTF 0.479, 핵심어 재현율 81.19%입니다. 추가로 AI Hub 109·463·464·71698
Training 원본에서 3,574개 발화를 선별했습니다. 긴 녹음은 라벨 시각으로 분할하고
44.1kHz 음원은 16kHz로 변환했으며, 읽기와 구간 검사를 통과한 5,050개 고유 발화
(중복 가중 포함 6,603행, 8.51시간)를 C 드라이브의 로컬 캐시로 만들었습니다.

전체 보강 v3은 CER 12.84%, 핵심어 재현율 81.19%로 채택하지 않았습니다. v2에서
출발해 조명·현관문·화장실·회의·환기만 저학습률로 1 epoch 보강한 v4 Q5_0은 CER
11.02%, RTF 0.641, 핵심어 재현율 82.18%로 현재 PC 최선 후보입니다. 그러나 핵심어
90% 목표에는 미달하므로 Pi 5 및 실제 대회용 마이크 검증 전에는 릴리스하지 않습니다.

## PC 실행

```powershell
cd "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\speech-to-text"
& ".\.venv\Scripts\homemind-stt.exe" transcribe `
  --backend whisper `
  --model-dir ".\models\whisper-bin-x64-b4938\Release" `
  --model-file ".\models\ggml-small.bin" `
  --threads 8 `
  "<16kHz-mono-PCM16.wav>"
```

Vosk partial과 Whisper final을 한 utterance ID로 묶으려면:

```powershell
& ".\.venv\Scripts\homemind-stt.exe" cascade `
  --partial-model-dir ".\models\vosk-model-small-ko-0.22" `
  --final-backend whisper `
  --final-model-dir ".\models\whisper-bin-x64-b4938\Release" `
  --final-model-file ".\models\ggml-small.bin" `
  --threads 8 `
  "<16kHz-mono-PCM16.wav>"
```

## 원본 데이터 파인튜닝

학습은 AI Hub 71405의 Training `가정환경/근거리/-N` 중 `domain=IoT`인 원본
15,281개(12.19시간)를 전부 사용합니다. ID 중복과 누락 음성은 0개이며,
Validation 100개와 겹치는 ID도 0개입니다.

```powershell
& ".\tools\build_training_manifest.ps1"
& ".\tools\start_wsl_whisper_lora.ps1" -Setup -Smoke
& ".\tools\start_wsl_whisper_lora.ps1"
& ".\tools\convert_and_quantize_whisper.ps1" -Quantization q5_0
& ".\tools\build_targeted_v2_manifest.ps1"
& ".\tools\start_wsl_whisper_targeted_v2.ps1"
& ".\tools\convert_and_quantize_whisper.ps1" -TrainingRun "whisper-small-homemind-lora-v2" -Quantization q5_0
& ".\tools\start_wsl_whisper_keyword_v3.ps1"
& ".\tools\convert_and_quantize_whisper.ps1" -TrainingRun "whisper-small-homemind-lora-v3" -Quantization q5_0
& ".\tools\start_wsl_whisper_focused_v4.ps1"
& ".\tools\convert_and_quantize_whisper.ps1" -TrainingRun "whisper-small-homemind-lora-v4" -Quantization q5_0
```

3090 학습 설정은 Whisper small LoRA, FP16/TF32, batch 8, gradient accumulation 2,
DataLoader worker 8, prefetch 4입니다. 학습 후 adapter와 병합한 Transformers 모델을
둘 다 보존합니다. 이어서 whisper.cpp 변환·양자화와 같은 100개 검증 재측정을 해야
하며, Pi 5 실측과 대회용 마이크 독립 테스트 전에는 릴리스로 승격하지 않습니다.

## 실제 마이크 acceptance 녹음 (P8, 사람이 녹음)

대회용 USB 마이크로 HomeMind 명령 문장 39개(조명·현관문·화장실·회의·환기 중심 + 시연·안전 문장,
`config/acceptance-real-mic-v1.json`)를 읽어 녹음하고, 같은 평가기로 핵심어 재현율 90%를 다시 잰다.
기존 AI Hub 100개 평가셋은 그대로 둔다. 녹음 파일과 manifest는 git 밖(로컬 전용)에 둔다.

```powershell
$py = "..\..\runtime\.venv\Scripts\python.exe"
& $py tools\record_acceptance.py --list-devices
& $py tools\record_acceptance.py --out E:\HomeMind-Datasets\recordings\real-mic-v1 --speaker S01 --distance near_0_5m --device <번호>
& $py tools\record_acceptance.py --out E:\HomeMind-Datasets\recordings\real-mic-v1 --speaker S01 --distance far_2m --device <번호>
& ".\.venv\Scripts\homemind-stt.exe" eval --backend whisper --model-dir .\models\whisper-bin-x64-b4938\Release `
  --model-file .\artifacts\training\whisper-small-homemind-lora-v4\ggml-whisper-small-homemind-lora-v4-q5_0.bin `
  --manifest E:\HomeMind-Datasets\recordings\real-mic-v1\manifest.jsonl --report E:\HomeMind-Datasets\recordings\real-mic-v1\report-v4.json
```
화자 코드는 이름 대신 `S01` 같은 익명 코드. 중간에 끊겨도 같은 명령으로 이어서 녹음된다.
2026-10-22 도구 점검: 녹음 함수를 AI Hub 음성 3개로 대체해 manifest → eval까지 통과(CER 3.0%, 핵심어 3/3).
**사람 녹음 결과는 아직 없음.**
