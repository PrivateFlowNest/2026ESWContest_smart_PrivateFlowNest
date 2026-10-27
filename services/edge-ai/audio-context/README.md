# HomeMind Audio Context

노션 `WBS-020`과 확정 결정 `DEC-024`의 PC 기준선입니다. VAD/YAMNet 뒤에
두 개의 작은 분류기를 따로 두며, EXAONE은 결과 설명과 Task JSON 구조화를
보조합니다.

- `activity`: 지속 생활 맥락 (`TV_MEDIA`, `IN_PERSON_MEETING` 등)
- `sound_event`: 순간 이벤트 (`DOORBELL`, `WASHER_COMPLETE`, `ALARM` 등)

모델은 장치를 직접 제어하지 않습니다. 출력은 `contracts/audio-context.schema.json` 형식으로 Pi 4 HomeMind Core에 전달되고, Pi 4가 정책을 적용합니다.

## PC 설치

PowerShell에서 이 폴더로 이동한 뒤 프로젝트 전용 환경을 만듭니다.

```powershell
python -m venv .venv
.\.venv\Scripts\Activate.ps1
python -m pip install --upgrade pip
python -m pip install -e ".[dev]"
```

TensorFlow Hub의 첫 실행에는 YAMNet 다운로드를 위한 인터넷 연결이 필요합니다. 입력 WAV는 공식 YAMNet 규격에 맞춰 코드에서 모노 16kHz, `[-1, 1]` 범위로 변환합니다.

RTX 3090 특징 추출은 WSL2의 별도 환경을 사용합니다. TensorFlow 2.11 이후
Windows 네이티브 CUDA는 지원되지 않으므로 Windows `.venv`는 개발·검사용으로
유지하고, 실제 대량 YAMNet 처리는 WSL2에서 실행합니다.

```bash
python3 -m venv ~/venvs/homemind-audio
source ~/venvs/homemind-audio/bin/activate
python -m pip install --upgrade pip
python -m pip install -e '/mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/audio-context[dev]' 'tensorflow[and-cuda]>=2.16,<2.21'
export TFHUB_CACHE_DIR="$HOME/homemind-cache/tfhub"
python -c "import tensorflow as tf; print(tf.config.list_physical_devices('GPU'))"
```

## 데이터 준비

### AI-Hub 대용량 ZIP 관리

E 드라이브에 받은 원천 ZIP을 압축 해제하기 전에 전체 목록과 구조를 검사합니다.

```powershell
homemind-audio archives scan `
  --root "E:\HomeMind-Datasets\raw\aihub"
```

내부 파일의 CRC까지 확인하려면 전체 검사를 실행합니다. 압축을 풀거나 ZIP을
삭제하지 않으며, 손상 ZIP이 발견되어도 나머지 검사를 계속합니다. HDD에서는
모든 압축 데이터를 읽기 때문에 수 시간이 걸릴 수 있습니다.

```powershell
homemind-audio archives verify `
  --root "E:\HomeMind-Datasets\raw\aihub" `
  --all
```

특정 ZIP 하나를 안전하게 압축 해제합니다. 결과는 데이터셋 아래의
`extracted\training|validation\ZIP이름`에 생성됩니다. 완료된 폴더에는
`.homemind-extracted.json` 검증 표식이 남으며, 같은 명령을 다시 실행하면
중복 해제를 건너뜁니다.

```powershell
homemind-audio archives extract `
  --root "E:\HomeMind-Datasets\raw\aihub" `
  --dataset "71405_noisy_command" `
  --archive "TS_01.다화자_소음_환경_003.가정환경_01.원거리.zip"
```

기본값은 원본 ZIP을 삭제하지 않습니다. 공간 확보를 위해 검증된 ZIP을 삭제하려면
아래의 두 옵션을 모두 명시해야 합니다. 이 동작은 복구할 수 없습니다.

```powershell
homemind-audio archives extract `
  --root "E:\HomeMind-Datasets\raw\aihub" `
  --dataset "71405_noisy_command" `
  --archive "TS_01.다화자_소음_환경_003.가정환경_01.원거리.zip" `
  --delete-archive-after-verify `
  --confirm-delete-originals DELETE_VERIFIED_ORIGINALS
```

반디집이 설치된 Windows PC에서는 Python 기본 해제기 대신 반디집 콘솔 도구를
자동화할 수 있습니다. 기존 CRC/WAV 검사, C 스테이징, E 전송 확인, 정상 ZIP만
삭제하는 절차는 그대로 유지됩니다.

```powershell
homemind-audio archives extract `
  --root "E:\HomeMind-Datasets\raw\aihub" `
  --all `
  --extractor bandizip `
  --staging-root "C:\HomeMind-Archive-Staging" `
  --minimum-free-gb 100 `
  --continue-on-error `
  --delete-archive-after-verify `
  --confirm-delete-originals DELETE_VERIFIED_ORIGINALS
```

여러 ZIP을 순차 처리할 때는 `--archive`를 반복하거나, 데이터셋 전체에 한해
`--all`을 사용할 수 있습니다. 압축 해제 도중 실패하면 `.partial` 폴더를 남기고
원본 ZIP은 건드리지 않습니다. 각 ZIP을 풀기 전에 필요한 해제 용량과 기본 20GB의
여유 공간을 확인하며, 필요하면 `--minimum-free-gb`로 보존할 공간을 늘릴 수 있습니다.

원본과 최종 데이터가 HDD에 있고 C 드라이브가 빠른 SSD라면, ZIP 하나씩 C에 임시
해제한 뒤 검증된 결과만 E로 전송할 수 있습니다. `--continue-on-error`를 함께 쓰면
CRC가 손상된 ZIP은 원본을 유지하고 C의 불완전 임시 데이터만 정리한 뒤 다음 ZIP으로
계속 진행합니다. 반면 디스크 공간 부족이나 E 전송 실패는 데이터 보호를 위해 즉시
중단합니다.

```powershell
homemind-audio archives extract `
  --root "E:\HomeMind-Datasets\raw\aihub" `
  --all `
  --staging-root "C:\HomeMind-Archive-Staging" `
  --minimum-free-gb 100 `
  --continue-on-error `
  --delete-archive-after-verify `
  --confirm-delete-originals DELETE_VERIFIED_ORIGINALS
```

`data/manifest.example.csv`를 `data/manifest.csv`로 복사하고 실제 WAV 경로를 채웁니다. 원본 음성은 Git에 커밋하지 않습니다.

현재 내려받은 AI Hub 원본에서 재현 가능한 파일럿 매니페스트를 자동 생성할 수
있습니다. WAV를 복사하거나 자르지 않고 원본 경로와 8초 구간만 CSV에 기록합니다.
같은 회의 녹음과 같은 화자가 학습/검증 양쪽에 들어가지 않도록 그룹 단위로
분할합니다.

```powershell
homemind-audio dataset build-aihub-manifest `
  --root "E:\HomeMind-Datasets\raw\aihub" `
  --output data\aihub-pilot.csv `
  --max-per-label 200 `
  --seed 42
```

이 파일럿에서 데이터 역할은 다음과 같습니다.

- `464`: `IN_PERSON_MEETING`, `ZOOM_SPEAKER`
- `463`: `TV_MEDIA`
- `109`: `SELF_TALK` (화자 단위 재분할)
- `71698` 대화: 회의 오탐 억제용 `UNKNOWN`
- `71698` 환경음: `ALARM`, `OTHER`
- `71405`: 이 모델에서 제외하고 TFLite/ONNX 부분 의도 모델용으로 보존

현재 공개 원본만으로 채워지지 않는 `QUIET`, `ZOOM_HEADPHONES`, `DOORBELL`,
`WASHER_COMPLETE`는 현장 녹음 또는 별도 허가 데이터로 보충해야 합니다. 생성 시
`aihub-pilot.report.json`에 누락 클래스와 제외한 손상 ZIP 5개가 기록됩니다.

공개 데이터 v3 보강은 다음 원칙을 사용합니다.

- TAU Sound Events and Speech Privacy Preservation의 음성 혼합 `doorbell`은
  `DOORBELL`에 추가합니다. FSD50K 파생 데이터이므로 원본 `fsd50k:<id>` 그룹과
  기존 split을 재사용하고, FSD50K eval 파생본은 제외합니다.
- ReaLISED의 세탁기 작동음·TV·문·생활 소리는 `OTHER` 하드 네거티브로만 사용합니다.
  세탁기 작동음은 완료 알림음이 아니므로 `WASHER_COMPLETE`로 매핑하지 않습니다.
- TAU의 CC BY-NC 4.0과 ReaLISED의 CC BY 4.0 출처 정보를 매니페스트에 보존합니다.

압축 해제 후 v3 매니페스트는 다음처럼 생성합니다.

```powershell
homemind-audio dataset build-tau-manifest `
  --root "E:\HomeMind-Datasets\raw\zenodo\10378145_tau_sound_privacy\extracted" `
  --metadata "E:\HomeMind-Datasets\raw\zenodo\10378145_tau_sound_privacy\metadata.csv" `
  --reference-manifest data\fsd50k.csv `
  --output data\tau-doorbell.csv `
  --max-per-label 0

homemind-audio dataset build-realised-manifest `
  --root "E:\HomeMind-Datasets\raw\zenodo\6488321_realised\extracted" `
  --output data\realised-hard-negatives.csv `
  --max-per-label 0
```

그 뒤 기존 `public-full.csv`와 두 파일을 병합하여 `public-full-v3.csv`를 만들고,
`--balance-datasets`를 켜 `public-balanced-v3.csv`를 생성합니다.

최종 학습 전에는 구조 완료 게이트를 실행합니다.

```powershell
homemind-audio readiness --manifest data\final-manifest.csv
```

기본 게이트는 필수 라벨별 고유 원본 train 80, validation 15, test 20개,
split 누수 0건을 요구합니다. 이후 독립 test Macro F1 0.90과 Pi 5 성능 시험까지
통과해야 최종 모델입니다. 세부 수집 규격은 `docs/FINAL_TRAINING_PLAN.md`에 있습니다.

FSD50K 원본을 공식 구조로 압축 해제한 뒤 초인종·경보 매니페스트를 생성할 수
있습니다. 기본값은 CC0/CC-BY만 사용하며 저작자, 라이선스, 원본 URL을 보존합니다.

```powershell
homemind-audio dataset build-fsd50k-manifest `
  --root "E:\HomeMind-Datasets\raw\fsd50k" `
  --output data\fsd50k.csv `
  --max-per-label 0
```

AI Hub 전체 목록과 FSD50K 목록은 원본 WAV를 복사하지 않고 하나의 공개데이터
매니페스트로 스트리밍 병합합니다. FSD50K의 라이선스·저작자·URL 필드는 유지되고,
완전히 같은 행은 한 번만 기록됩니다.

```powershell
homemind-audio manifest merge `
  --input data\aihub-full.csv `
  --input data\fsd50k.csv `
  --output data\public-full.csv
```

원본 목록에서 파일 존재 검사를 이미 완료했다면 병합 직후의 빠른 구조 검사는
`--skip-file-check`로 실행할 수 있습니다. 최종 배포 전에는 이 옵션 없이 다시
전수 검사합니다. 준비도 최소 수량은 CSV 조각 수가 아니라 고유 `group_id` 수로
계산합니다.

```powershell
homemind-audio readiness `
  --manifest data\public-full.csv `
  --skip-file-check `
  --report data\public-full.readiness.json
```

전체 인덱스는 보존하되 실제 특징 추출은 원본 그룹 다양성을 우선하는 균형 목록으로
수행합니다. 기본값은 라벨별 train 4,000행, validation/test 각 1,000행과 원본
그룹당 최대 4개 조각입니다.

```powershell
homemind-audio manifest balance `
  --input data\public-full.csv `
  --output data\public-balanced-v1.csv `
  --max-train-per-label 4000 `
  --max-validation-per-label 1000 `
  --max-test-per-label 1000 `
  --max-segments-per-group 4 `
  --seed 42
```

서로 다른 공개 데이터셋이 한 라벨에 섞인 경우 큰 데이터셋이 선택 결과를 독점하지
않도록 v2에서는 데이터셋 단위 라운드로빈을 사용합니다. 결과 파일은 계속 경로와
구간 시작순으로 정렬되어 HDD 순차 읽기 지역성을 유지합니다.

```powershell
homemind-audio manifest balance `
  --input data\public-full.csv `
  --output data\public-balanced-v2.csv `
  --balance-datasets `
  --seed 42
```

PC 마이크로 한 클립을 녹음하면서 manifest에 자동 등록할 수 있습니다.

먼저 입력 장치 번호를 확인합니다.

```powershell
homemind-audio devices
```

```powershell
homemind-audio record `
  --head activity `
  --label TV_MEDIA `
  --seconds 8 `
  --device 1 `
  --output data\raw\activity\tv_media\tv_001.wav
```

순간 이벤트는 `--head sound_event --label DOORBELL`처럼 기록합니다. 녹음을 시작하기 전에 바로 소리를 내지 말고, 명령 실행 후 표시되는 `Recording...`을 확인합니다.

```csv
path,head,label,split
raw/activity/tv_media/tv_001.wav,activity,TV_MEDIA,train
raw/activity/meeting/meeting_001.wav,activity,IN_PERSON_MEETING,train
raw/event/doorbell/doorbell_001.wav,sound_event,DOORBELL,train
```

지속 맥락은 5~10초, 초인종·알람 같은 순간 이벤트는 앞뒤 여백을 포함한 1~3초 클립으로 시작합니다. 같은 녹음 원본에서 잘라낸 조각은 서로 다른 train/test split에 섞지 않습니다.

## 학습

```powershell
homemind-audio train `
  --manifest data\public-balanced-v1.csv `
  --output artifacts\public-balanced-v1 `
  --cache cache\public-balanced-v1.sqlite3 `
  --skip-file-check
```

특징 캐시는 처리한 YAMNet 벡터를 SQLite에 즉시 누적하므로 학습이 중단되어도 다음
실행에서 완료된 항목을 다시 계산하지 않습니다. validation과 test 결과는 서로 합치지
않고 `metrics.json`에 별도 기록합니다.

v2 특징 추출은 CPU가 다음 WAV를 미리 읽고 리샘플링하는 동안 GPU가 현재 배치를
처리합니다. 먼저 실제 GPU에서 배치 크기별 속도와 단일 처리 결과 동등성을 측정합니다.

```powershell
wsl -d Ubuntu-22.04 -- bash -lc "source ~/venvs/homemind-audio/bin/activate && cd /mnt/c/Users/alexi/Desktop/Workspace/ESW/services/edge-ai/audio-context && homemind-audio benchmark --manifest data/public-balanced-v2.csv --samples 32 --batch-sizes 1,2,4,8,16 --prefetch-workers 2 --skip-file-check --output data/rtx3090-batch-benchmark.json"
```

현재 RTX 3090의 혼합 길이 32개 측정에서는 배치 8이 선택됐습니다. 모든 후보의
특징이 단일 처리와 허용오차 내에서 같을 때만 선택 후보가 됩니다. HDD에서는 과도한
랜덤 읽기를 피하도록 프리패치 worker 2개와 read-ahead 2배치를 기본으로 사용합니다.

WSL GPU 본 학습은 Windows PowerShell에서 다음처럼 백그라운드로 시작합니다.
PID와 진행 로그가 `logs/`에 남고, 같은 스크립트를 다시 실행해도 기존 프로세스가
살아 있으면 중복 학습을 시작하지 않습니다.

```powershell
& .\tools\start_wsl_gpu_training.ps1
```

v2 데이터 균형과 배치 추출을 사용하는 재학습은 별도 로그·결과 폴더를 사용합니다.

```powershell
& .\tools\start_wsl_gpu_training_v2.ps1
```

v4는 YAMNet 프레임의 평균만 쓰던 기준선과 달리 평균·표준편차·최대값을 함께
보존합니다. 짧은 초인종 피크와 TV/회의의 시간 변화를 분류기가 사용할 수 있으며,
기존 v1-v3 모델과 캐시는 그대로 유지됩니다. Validation Macro-F1으로
Logistic Regression의 `C`와 class weight를 선택한 뒤 고정된 모델로 test를
평가합니다. RTX 3090의 v4 32개 벤치마크에서도 배치 8이 선택됐고 모든 배치가
단일 처리와 최대 절대 오차 0으로 일치했습니다.

터미널에서 진행률을 보며 실행하려면:

```powershell
& .\tools\start_wsl_gpu_training_v4.ps1
```

백그라운드로 실행하려면:

```powershell
& .\tools\start_wsl_gpu_training_v4.ps1 -Background
```

v4 결과와 전용 특징 캐시는 각각 `artifacts/public-balanced-v4-temporal`과
`~/homemind-cache/public-balanced-v4-temporal.sqlite3`에 생성됩니다.

v5는 학습 입력 길이를 Pi 5 실시간 추론의 4초 창과 일치시킵니다. 기존 8초 활동
구간을 4초 창·2초 간격으로 다시 인덱싱하며 원본 `group_id`와 split을 그대로
유지합니다. 4초 미만 활동 구간은 제외하고, `sound_event` 행은 변경하지 않습니다.

```powershell
homemind-audio manifest windowize `
  --input data\public-balanced-v3.csv `
  --output data\public-balanced-v5-window4s.csv `
  --head activity `
  --window-seconds 4 `
  --hop-seconds 2 `
  --short-policy drop

& .\tools\start_wsl_gpu_training_v5.ps1 -Background
```

v5는 v4와 같은 YAMNet 특징 정의를 사용하므로 정확한 파일·시간 구간이 같은 항목은
v4 특징 캐시를 안전하게 재사용합니다. 모델 결과는
`artifacts/public-balanced-v5-window4s`에, 진행 로그는
`logs/public-balanced-v5-window4s.wsl.*.log`에 기록됩니다. 공개 validation은
후보 비교용이며, 최종 성능 판정은 실제 Pi 5와 대회용 마이크로 별도 수집한 test에서
수행합니다.

v6는 모델 선택 전에 validation의 원본 그룹을 별도 `test` split으로 예약해 최종
평가 누수를 막고, TV/회의 오류 비용을 다르게 준 활동 분류기를 학습한다. 전체 비용
후보 검색과 고재현율 후보 재현은 각각 다음 스크립트로 실행한다.

```powershell
& .\tools\start_wsl_gpu_training_v6.ps1
& .\tools\start_wsl_gpu_training_v6_recall.ps1
```

최종 v6.2 활동 모델은 보수형과 고재현율형의 확률을 50:50으로 평균한다. 두 분류기는
동일한 YAMNet 특징을 공유하므로 YAMNet 추론은 한 번뿐이다.

```powershell
homemind-audio ensemble-activity `
  --model artifacts\public-balanced-v6-cost-sensitive\activity.joblib `
  --model artifacts\public-balanced-v6-recall-buffer\activity.joblib `
  --weights 1,1 `
  --output artifacts\public-balanced-v6-ensemble-50-50\activity.joblib
```

그룹 분리 test의 융합 정책 결과는 정확도 88.0%, 조용 모드 재현율 91.33%, 오탐률
4.89%, 활성 p95 7초, 해제 대용치 p95 5초로 노션 CTX-003 게이트를 통과했다.
실제 Pi 5와 대회용 마이크로 수집할 마지막 현장 test는 여전히 별도로 필요하다.

결과:

- `artifacts/activity.joblib`
- `artifacts/sound_event.joblib`
- `artifacts/metrics.json`

`metrics.json`에는 클래스별 precision/recall/F1뿐 아니라 혼동행렬과 오탐률도
저장됩니다. 최초 AI Hub 파일럿은 그룹 누수 없는 검증에서 활동 맥락 정확도
81.66%, macro F1 81.67%, 경보음 정확도 93.45%, macro F1 92.54%를 기록했습니다.
활동 맥락은 아직 노션 목표 90%에 도달하지 않았으므로 최종 모델이 아니라
재현 가능한 기준선입니다.

## 단일 WAV 예측

```powershell
homemind-audio predict sample.wav --model artifacts\activity.joblib
```

회의 계열로 판단되면 출력의 `policy_tags`에 `QUIET_REQUIRED`가 포함됩니다. 초인종 같은 순간 이벤트는 별도 `SOUND_EVENT`로 출력됩니다.

## 하이브리드 실시간 추론

노션의 자동 맥락 세로 슬라이스에 맞춰 활동은 v6.2 앙상블 모델, 순간 음향은 v5
모델을 함께 사용합니다. 같은 YAMNet 특징을 한 번만 계산하므로 YAMNet을 중복
실행하지 않습니다.

PC의 기본 마이크로 실행:

```powershell
& .\.venv\Scripts\homemind-audio.exe live `
  --activity-model artifacts\public-balanced-v6-ensemble-50-50\activity.joblib `
  --sound-model artifacts\public-balanced-v5-window4s\sound_event.joblib
```

마이크가 없어도 WAV/FLAC 파일을 겹치는 4초 창으로 재생해 전체 경로를 시험할 수
있습니다.

```powershell
& .\.venv\Scripts\homemind-audio.exe live `
  --activity-model artifacts\public-balanced-v6-ensemble-50-50\activity.joblib `
  --sound-model artifacts\public-balanced-v5-window4s\sound_event.joblib `
  --wav "C:\audio\meeting.wav" `
  --max-windows 10
```

각 창의 원시 예측은 진행 확인용으로 stderr에 표시되고, Pi 4에 전달 가능한
`audio-context.schema.json` JSON 이벤트만 stdout에 한 줄씩 출력됩니다. 기본 동작은
다음과 같습니다.

- 활동 confidence 0.65 미만은 `UNKNOWN`
- `QUIET_REQUIRED` 진입은 3개 연속 창, 해제는 2개 연속 창
- 활성 맥락은 5초마다 갱신하며 TTL은 10초
- `OTHER` 음향은 내보내지 않고, 나머지는 confidence 0.80 이상이 2회 연속일 때 출력
- 같은 순간 음향 이벤트는 8초 동안 중복 억제

`context-fusion`으로 연결할 때는 기존 안정화 출력을 다시 안정화하지 않고, 매 오디오
창의 맥락·VAD 비율을 새 특징 계약으로 출력한다.

```powershell
& .\.venv\Scripts\homemind-audio.exe live `
  --activity-model artifacts\public-balanced-v4-temporal\activity.joblib `
  --sound-model artifacts\public-balanced-v4-temporal\sound_event.joblib `
  --feature-events
```

실제 마이크일 때는 `simulated=false`, `--wav` 재생일 때는 `simulated=true`가 자동으로
기록된다. 이 모드의 표준 출력은
`contracts/context-feature-observation.schema.json`의 `AUDIO_CONTEXT` 이벤트만
포함하므로 `context-fusion stream-replay -`에 바로 연결할 수 있다.

이 숫자는 공개 데이터 기준의 안전한 시작값입니다. 실제 Pi 5와 대회용 마이크를
확보하면 TV 오탐률, 활성 지연, 해제 지연을 측정해 조정해야 합니다. 이 서비스는
가전 명령을 직접 만들지 않으며, 출력 이벤트를 받은 Pi 4의 결정론적 정책 엔진이
최종 실행 여부를 결정합니다.

## 테스트

```powershell
python -m pytest
```

현재 단계는 PC 학습·평가 기준선입니다. Pi 5 배포 단계에서는 YAMNet TFLite와 학습한 분류기 가중치를 하나의 경량 실행 경로로 내보냅니다.
