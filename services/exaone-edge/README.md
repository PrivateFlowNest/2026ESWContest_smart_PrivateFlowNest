# HomeMind EXAONE Edge

라즈베리파이 5에서 EXAONE 4.0 1.2B가 사용자 발화를 고정된 Task JSON으로 제안하도록 만드는 로컬 서비스입니다. 출력은 **실행 명령이 아니라 신뢰하지 않는 제안**이며, 라즈베리파이 4의 결정론적 정책 엔진이 허용된 기기와 동작을 다시 확인한 뒤에만 실행해야 합니다.

## 현재 안전 경계

- JSON Schema와 정확히 맞지 않는 출력은 거부하고 한 번만 재시도합니다.
- 모델 연결 실패나 재시도 실패 시 실행 가능한 필드를 비운 `CLARIFY`로 닫힙니다.
- 부정 명령, 불명확한 대상·동작, 미지원 기기, 프롬프트 주입은 모델보다 먼저 결정론적으로 차단합니다.
- 한 번에 한 요청만 처리해 Pi 5 메모리 사용을 제한합니다.
- 원본 음성·영상은 입력으로 보내지 않고 정제된 텍스트와 컨텍스트만 받습니다.

## PC에서 실행

llama.cpp 서버:

```powershell
& "C:\Users\alexi\AppData\Local\Microsoft\WinGet\Packages\ggml.llamacpp_Microsoft.Winget.Source_8wekyb3d8bbwe\llama-server.exe" -m ".\artifacts\exaone-1.2b-homemind-v1-q4_k_m.gguf" --alias "exaone-1.2b-homemind-v1" --host 127.0.0.1 --port 8080 -c 4096
```

별도 PowerShell에서:

```powershell
cd "C:\Users\alexi\Desktop\Workspace\ESW\services\exaone-edge"
python -m venv .venv
& ".\.venv\Scripts\python.exe" -m pip install -e .
& ".\.venv\Scripts\homemind-exaone.exe" --model "exaone-1.2b-homemind-v1" health
& ".\.venv\Scripts\homemind-exaone.exe" --model "exaone-1.2b-homemind-v1" interpret "거실 불을 켜줘" --debug
& ".\.venv\Scripts\homemind-exaone.exe" --model "exaone-1.2b-homemind-v1" eval --cases ".\data\smoke-cases.jsonl" --report ".\artifacts\q4-k-m-hardware-smoke-report.json"
```

계약은 `contracts/task-proposal.schema.json`, 실제 시연 기기 허용 목록은 `config/device-capabilities.json`에서 관리합니다.
논리 장치명은 Pi 4 경계에서 엑셀 기반 `hardware_id`로 해석합니다. 일반 조명은
사용자가 확정한 `LIGHT`, 현관문은 `SERVO-DOOR-01`, 화장실문은
`SERVO-DOOR-02`, 창문은 `SERVO-WINDOW-01`, 환기팬은 `FAN-01`입니다.
엑셀 원본의 X/Y 모터 ID는 둘 다 `MOTOR-XY-Y-01`로 중복되어 있으므로
임의 보정하지 않고 배선 전에 별도 확정합니다.

## YAMNet 회의 감지 연결

구현된 흐름은 다음과 같습니다.

1. `homemind-audio live`가 4초 음성 창을 분석합니다.
2. `context-fusion`이 VAD와 오디오·비전·기기 상태를 융합하고, 회의 계열 판정이 3회 연속 확인되면 `QUIET_REQUIRED` 컨텍스트를 냅니다.
3. `homemind-exaone bridge-audio`는 `pi5.context-fusion` 이벤트의 상태가 바뀐 순간에만 EXAONE을 호출합니다. heartbeat에는 다시 호출하지 않습니다. 이전 `pi5.audio-context` 계약도 전환 기간 동안 호환합니다.
4. 회의 시작은 `ALL_QUIET_DEVICES/SET_QUIET_MODE`, 종료는 `ALL_QUIET_DEVICES/RESUME` 제안으로 제한합니다. 다른 출력은 Pi 4로 전달하지 않습니다.
5. Pi 4는 `contracts/pi4-task-proposal-envelope.schema.json`과 자체 기기 상태·정책을 검사한 뒤 실행합니다.

PC에서 저장된 이벤트로 전체 연결을 확인하는 명령:

```powershell
cd "C:\Users\alexi\Desktop\Workspace\ESW\services\exaone-edge"
& ".\.venv\Scripts\homemind-exaone.exe" --model "exaone-1.2b-homemind-v1" bridge-audio --events ".\data\audio-bridge-smoke-events.jsonl"
```

`context-fusion`이 저장한 새 계약 이벤트를 재생하고 제안 envelope도 보존하려면:

```powershell
& ".\.venv\Scripts\homemind-exaone.exe" `
  --model "exaone-1.2b-homemind-v1" `
  bridge-audio `
  --events "..\edge-ai\context-fusion\artifacts\real-exaone-bridge-events.jsonl" `
  --output ".\artifacts\real-context-bridge-proposals.jsonl"
```

브리지는 기본적으로 시작할 때 결과를 폐기하는 안전한 EXAONE 워밍업을 한 번 수행한다.
따라서 첫 실제 `QUIET_REQUIRED` 이벤트가 모델의 콜드 추론 비용을 대신 부담하지
않는다. 진단 목적으로만 `--no-warmup`을 사용할 수 있다.

마이크가 준비된 뒤 오디오→맥락 융합→EXAONE을 연결하는 명령(각 서비스의
가상환경 경로 사용):

```powershell
& "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\audio-context\.venv\Scripts\homemind-audio.exe" live `
  --activity-model "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\audio-context\artifacts\public-balanced-v4-temporal\activity.joblib" `
  --sound-model "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\audio-context\artifacts\public-balanced-v4-temporal\sound_event.joblib" `
  --feature-events |
& "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe" `
  stream-replay - |
& "C:\Users\alexi\Desktop\Workspace\ESW\services\exaone-edge\.venv\Scripts\homemind-exaone.exe" `
  --model "exaone-1.2b-homemind-v1" bridge-audio
```

위 명령은 Pi 4가 없을 때 검증된 제안 envelope를 화면에 출력합니다. Pi 4 정책 API가 준비되면 마지막 줄에 다음 옵션을 추가합니다.

```powershell
--pi4-url "http://<PI4-IP>:<PORT>/api/v1/task-proposals"
```

인증이 필요하면 `HOMEMIND_PI4_API_KEY` 환경 변수에 키를 설정합니다. 전송에 실패하면 같은 `proposal_id`를 재사용하고 15초 뒤 heartbeat에서 재전송하므로 Pi 4는 `Idempotency-Key`로 중복 실행을 막아야 합니다. 원본 음성은 이 경계를 통과하지 않습니다.

## 파인튜닝 판단 기준

20개 스모크 테스트는 연결·계약 확인용일 뿐 학습 결정에 충분하지 않습니다. 서로 겹치지 않는 최소 300개 발화로 다음을 측정합니다.

- 전체 파이프라인 의미 정확도 95% 이상
- 최종 계약 성공률 100% (실패 시 안전 fallback 포함)
- 모델 첫 출력 성공률 95% 이상
- 위험·부정·미지원 명령의 오실행 0건

전체 의미 정확도 또는 모델 첫 출력 성공률이 95% 미만이면 EXAONE 1.2B LoRA 파인튜닝을 진행합니다. 양자화 GGUF 자체를 학습하지 않고 원본 가중치에 LoRA를 학습한 뒤 병합·양자화합니다.

## LoRA v1 결과

- 학습: 1,311개, 검증: 810개, 3 epoch, RTX 3090, 약 14분
- 미사용 합성 테스트 810개: JSON 100%, 계약 99.88%, 의미 99.01%
- 남은 모델 단독 오류 8개는 미지원 공기청정기와 프롬프트 주입 경계 사례이며 실제 서비스의 결정론적 가드가 모델 호출 전에 차단합니다.
- Q4_K_M llama.cpp 하드웨어 스모크: 24/24, 모델 첫 출력 15/15
- 배포 파일: `artifacts/exaone-1.2b-homemind-v1-q4_k_m.gguf` (812,437,824 bytes)

이 수치는 합성 데이터 기준입니다. 모형과 마이크가 준비되면 팀원 자연 발화, ASR 오류가 포함된 문장, Pi 5 실측 지연을 별도 acceptance set으로 다시 평가해야 합니다.

## 자연 문장 독립 평가와 PC 종단간 게이트

학습 생성기와 분리해 직접 작성한 한국어 100문장으로 다시 평가했다. 모델 단독
결과는 의미 일치 59%, 첫 출력 계약 성공 79%, 검증 통과 86%이므로 EXAONE의
출력을 그대로 실행하면 안 된다. 실제 구성인 `결정론적 안전 가드 → EXAONE
제안 → 최종 가드 → Pi 4 정책`에서는 의미·계약·모델 첫 제안 계약이 모두
100/100이고, 안전 21/21, 명령 64/64, 맥락 10/10, 무동작 5/5를 통과했다.
최종 재검증 PC 지연은 P95 250.272ms였다.

엑셀 사진의 실제 ID를 쓰는 PC 정책/컨트롤러 시뮬레이션도 10회 연속 통과했다.
각 요청은 동일 `proposal_id` 재전송 때 두 번 실행되지 않았고, ACK에는
`real_hardware_executed=false`가 명시된다. 일반 조명 `LIGHT`는 엑셀에
컨트롤러·채널이 없으므로 임의 연결하지 않고 실제 배선 확정 전까지 실행 경로에서
거부한다. X/Y 모터의 중복 `MOTOR-XY-Y-01`도 같은 원칙으로 거부한다.

```powershell
& ".\.venv\Scripts\homemind-exaone.exe" `
  --model "exaone-1.2b-homemind-v1" `
  --capabilities ".\config\device-capabilities.json" `
  e2e-sim `
  --cases ".\data\pc-e2e-cases-v1.jsonl" `
  --inventory ".\config\hardware-inventory.json" `
  --report ".\artifacts\pc-e2e-report-v1.json"
```

## Pi 5 스테이징 묶음

`artifacts/deploy/exaone-edge-v1-staging`에는 Q4_K_M 모델, 실행 wheel, 세 계약,
엑셀 기반 기기 목록, 자연 문장 평가, PC E2E 결과, 오디오 브리지 결과가 들어 있다.
10개 파일의 크기와 SHA-256은 검증됐다. 실제 Pi 5 성능·발열, 실제 Pi 4/ESP
ACK, 암호학적 서명이 남아 있어 활성화 상태는 의도적으로 `STAGING_ONLY`다.

```powershell
& ".\.venv\Scripts\homemind-exaone.exe" verify-package `
  ".\artifacts\deploy\exaone-edge-v1-staging"
```

학습·평가 명령:

```powershell
& ".\tools\start_wsl_lora_training.ps1" -Full
wsl.exe -d Ubuntu-22.04 -- bash /mnt/c/Users/alexi/Desktop/Workspace/ESW/services/exaone-edge/training/run_lora_evaluation.sh
```
