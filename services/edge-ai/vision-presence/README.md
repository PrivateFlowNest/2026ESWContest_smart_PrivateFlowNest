# HomeMind Vision Presence

내부 `CAM-INDOOR-01`과 외부 `CAM-OUTDOOR-01`에서 신원을 식별하지 않고 모형 인형 `FIGURE`의 위치만 검출하기 위한 서비스입니다.

## 후보 결정

- 최종 채택: YOLOX-Nano 416
- YOLOX-Tiny 416은 PC 비교 기록만 보존하며 실행에는 사용하지 않음
- 배포 형식: ONNX, Pi 5 CPU 실행은 ONNX Runtime + XNNPACK 우선
- 모델 출력: `FIGURE` bounding box 한 종류
- 공간 판정: 모델 클래스가 아니라 bounding box 하단 중앙점과 카메라별 polygon으로 결정

제조사 교육자료에 YOLOX용 ONNX Runtime·TFLite·EcoAI C++ 예제가 모두 있으므로 다른 모델군보다 실제 OS 통합 위험이 낮습니다.

## PC 준비 및 속도 비교

```powershell
cd "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\vision-presence"
python -m venv .venv --system-site-packages
& ".\.venv\Scripts\python.exe" -m pip install -e .
& ".\.venv\Scripts\python.exe" ".\tools\prepare_yolox_candidates.py"
& ".\.venv\Scripts\homemind-vision.exe" benchmark `
  ".\artifacts\candidates\yolox_nano_416.onnx" `
  ".\artifacts\candidates\yolox_tiny_416.onnx" `
  --threads 4 --streams 2 --warmup-runs 10 --runs 100
```

임시 사진으로 전처리·추론·YOLOX 디코딩·NMS·bounding box 출력을 확인할 수 있습니다.

```powershell
& ".\.venv\Scripts\homemind-vision.exe" detect `
  ".\artifacts\candidates\yolox_nano_416.onnx" `
  ".\data\surrogate\dog.jpg" `
  --output ".\artifacts\surrogate-nano.jpg"
```

PC 결과는 후보 간 상대적인 계산량과 배포 호환성만 판정합니다. 최종 인형이 정해지면 같은 촬영 데이터로 두 모델을 학습해 `small figure recall`, 오탐률, 구역 정확도를 비교하고, 마지막으로 Pi 5에서 UI·오디오·EXAONE과 30분 동시 부하를 측정해야 합니다.

`config/zones.template.json`의 polygon은 코드 검증용 예시일 뿐 실제 방 배치가 아닙니다.

현재 PC 측정 결과와 실제 인형 확정 후 검증 기준은 `artifacts/candidate-decision.md`에 기록되어 있습니다. 4스레드 CPU 기준 P95는 Nano 6.450ms, Tiny 30.565ms였으며 모델 크기는 각각 약 3.7MB와 20.3MB였습니다.

## 지금 완료된 것

- 단일 클래스 `FIGURE` 데이터셋 초기화와 로컬 저장
- 카메라 또는 동영상에서 일정 간격으로 프레임 수집
- 빈 배경 차이를 이용한 bounding box 초안 생성
- 사람이 확인한 `ACCEPTED`, 인형이 없는 `EMPTY`, 폐기할 `REJECTED` 기록
- 같은 촬영 세션이 서로 다른 split에 섞이지 않는 그룹 단위 분리
- 임시 인형(`SURROGATE`)을 train에만 허용하고 최종 검증·테스트에서 차단
- YOLOX용 COCO 변환, 학습 준비도 검사, RTX 3090 학습, test 평가, ONNX 내보내기

현재 생성된 `data/smoke-dataset`은 코드 시험용 가짜 데이터입니다. 실제 모델 학습 성능을 의미하지 않습니다.

## 인형 확정 전 확인

아래 명령은 가짜 막대 인형으로 촬영부터 ONNX 입력까지의 형식을 연습합니다.

```powershell
cd "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\vision-presence"
& ".\.venv\Scripts\python.exe" ".\tools\create_smoke_dataset.py"
```

RTX 3090에서 실제 순전파·역전파 한 회를 확인하려면 다음을 실행합니다.

```powershell
$env:PYTHONPATH = "$PWD\.cache\torch\hub\Megvii-BaseDetection_YOLOX_main;$PWD\src"
& ".\.venv\Scripts\python.exe" ".\tools\smoke_training_step.py" `
  --experiment ".\training\yolox_nano_figure.py" `
  --coco-dir ".\artifacts\smoke-coco" `
  --batch-size 128
```

이 PC에서는 batch 128의 forward/backward가 통과했고 peak VRAM은 약 13,281MiB였습니다. 본 학습은 FP16을 사용하므로 3090 24GB에서 이 값을 시작점으로 삼습니다. 메모리 부족이 생기면 `-BatchSize 64`로 낮춥니다.

## 최종 인형 확정 후 데이터 수집

먼저 데이터셋을 만듭니다.

```powershell
$cli = ".\.venv\Scripts\homemind-vision.exe"
$dataset = ".\data\figure-dataset"
& $cli dataset init --root $dataset
```

인형을 치운 상태에서 카메라마다 빈 배경을 한 장 저장합니다. Windows에서 장치 번호가 각각 `0`, `1`이라고 가정한 예시입니다.

```powershell
& $cli dataset background --root $dataset --camera-id CAM-INDOOR-01 --source 0
& $cli dataset background --root $dataset --camera-id CAM-OUTDOOR-01 --source 1
```

카메라 대신 녹화 파일을 써도 됩니다. 실제로 사용할 인형·방·카메라로 찍은 사진만 `FINAL`입니다. 임시 인형은 반드시 `SURROGATE`로 지정합니다.

```powershell
& $cli dataset capture --root $dataset `
  --source ".\captures\indoor-session-01.mp4" `
  --camera-id CAM-INDOOR-01 --source-kind FINAL `
  --group-id "INDOOR-DAY-01" --every-seconds 0.5 --max-frames 100 `
  --lighting NORMAL --distance-band MID --occlusion NONE
```

`group-id`는 같은 연속 촬영끼리만 같게 둡니다. 조명, 거리, 배치 또는 촬영 날짜를 바꿨다면 새 ID를 씁니다. 최종 권장 최소치는 총 train/validation/test 300/75/75장이고, 각 카메라가 각 split에 150/35/35장 이상 포함되어야 합니다. 각 split의 10% 이상은 인형이 없는 `EMPTY` 이미지로 수집합니다.

## 자동 박스 초안과 검수

```powershell
& $cli dataset propose --root $dataset
& $cli dataset preview --root $dataset
```

`data/figure-dataset/previews`의 초록/노란 박스를 눈으로 확인합니다. 파일명 자체가 `sample-id`입니다.

```powershell
# 제안 박스가 맞는 경우
& $cli dataset review --root $dataset --sample-id "vision-..." --decision ACCEPTED

# 인형이 없는 정상 영상 프레임(오탐 방지용 negative)
& $cli dataset review --root $dataset --sample-id "vision-..." --decision EMPTY

# 박스를 직접 바로잡는 경우: X1 Y1 X2 Y2
& $cli dataset review --root $dataset --sample-id "vision-..." `
  --decision ACCEPTED --bbox 42 31 108 205

# 흔들림·가림 등으로 학습에 쓰면 안 되는 프레임
& $cli dataset review --root $dataset --sample-id "vision-..." --decision REJECTED
```

`accept-proposed`는 가짜 smoke 데이터 전용입니다. 실제 `FINAL` 촬영본에는 사용하지 않습니다.

## 고정 분리, 준비도 검사, 본 학습

모든 촬영과 검수가 끝난 뒤 한 번에 수행합니다. 이후 데이터를 추가했다면 split과 COCO export를 다시 만들고 준비도를 재검사합니다.

```powershell
& ".\.venv\Scripts\python.exe" -m pip install -e ".[train]"
& $cli dataset split --root $dataset
& $cli dataset ready --root $dataset
& $cli dataset export-coco --root $dataset --output ".\data\figure-coco"
& ".\tools\start_training.ps1" -BatchSize 128 -Epochs 150
```

학습 중에는 validation만 사용합니다. 모델 선택이 완전히 끝난 뒤 test를 한 번 평가하고 ONNX로 내보냅니다.

```powershell
& ".\tools\evaluate_test_set.ps1" -BatchSize 128
& ".\tools\export_trained_model.ps1"
```

최종 ONNX는 `artifacts/homemind-yolox-nano-figure-v1.onnx`에 생성되며, 원본 영상과 사진은 외부로 전송하지 않습니다.
