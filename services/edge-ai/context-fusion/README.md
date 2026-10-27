# HomeMind context-fusion

PC에서 먼저 검증하고 Raspberry Pi 5로 옮길 수 있는 상황 융합 서비스다. 노션의
WBS-046과 REQ-041을 기준으로 오디오 문맥, 발화 비율, 화자 전환,
지속시간, 사람의 공간 구역, TV·노트북·헤드폰 상태를 결합한다.

## 책임 경계

- 출력 클래스: `QUIET`, `SELF_TALK`, `TV_MEDIA`, `ZOOM_SPEAKER`,
  `ZOOM_HEADPHONES`, `IN_PERSON_MEETING` (`UNKNOWN`은 불확실 상태)
- `ZOOM_SPEAKER`, `ZOOM_HEADPHONES`, `IN_PERSON_MEETING`만
  오디오만으로 `quiet_required=true`가 된다. 안정화된 비전 구역에 사람이 있고
  `BED + QUIET`이면 수면, `DESK + QUIET`이면 공부로 보아 같은 상태를 적용한다.
- 3회 연속 관측 뒤 조용한 환경 필요 상태를 활성화하고, 2회 연속 해제 관측 뒤
  비활성화한다.
- 원본 음성, 영상, 좌표, 신원, 전사문은 입력·출력하지 않는다.
- 장치 명령은 출력할 수 없다. 이 서비스는 Pi 4 정책 엔진에 상황 제안만 보내며,
  실제 제어 권한은 Pi 4에 남는다.

## PC 실행

```powershell
cd C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\context-fusion
py -3.12 -m pip install -e ".[dev]"
homemind-context-fusion replay .\scenarios\demo.jsonl
```

EXAONE에 넘길 최소 정보만 확인하려면 다음처럼 실행한다.

```powershell
homemind-context-fusion replay .\scenarios\demo.jsonl --sanitized
```

테스트와 PC 지연시간 측정:

```powershell
py -3.12 -m pytest -p no:cacheprovider
homemind-context-fusion benchmark .\scenarios\demo.jsonl --runs 10000
```

## 실제 WAV에서 종단간 재생

YAMNet/TensorFlow가 설치된 기존 `audio-context` 가상환경을 사용한다. 표준 출력에는
Pi 4로 보낼 수 있는 계약 이벤트만 나오고, 창별 모델 예측과 지연은 별도 JSONL에
저장된다.

소스 체크아웃에서는 아래 설치를 한 번 수행한 뒤 콘솔 명령을 사용하는 편이 편하다.

```powershell
& .\services\edge-ai\audio-context\.venv\Scripts\python.exe -m pip install `
  -e ".\services\edge-ai\context-fusion"

& .\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe `
  audio-replay "C:\path\to\meeting.wav" `
  --presence-zone DESK `
  --debug-jsonl ".\services\edge-ai\context-fusion\artifacts\audio-replay.jsonl" `
  --events-jsonl ".\services\edge-ai\context-fusion\artifacts\context-events.jsonl" `
  --summary ".\services\edge-ai\context-fusion\artifacts\audio-replay-summary.json"
```

회의 감지 뒤 자동 해제까지 한 번에 확인하려면 테스트 전용 무음을 뒤에 붙인다.

```powershell
& .\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe `
  audio-replay "C:\path\to\meeting.wav" `
  --trailing-silence-seconds 8
```

`--presence-zone`, `--turns-per-minute`는 현재 PC 통합시험에서
나중의 비전·턴테이킹 출력 자리를 대신하는 입력이다. 대회 시연에서는
수동 회의 모드가 아니라 각 모듈이 이 값을 자동 공급해야 한다.

검증 manifest에서 라벨별 실제 파일을 뽑아 맥락 혼동행렬, `QUIET_REQUIRED`
재현율·오탐률과 활성 지연을 만들 수 있다.

```powershell
& .\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe `
  evaluate-audio `
  --manifest ".\services\edge-ai\audio-context\data\public-balanced-v3.csv" `
  --per-label 5 `
  --report ".\services\edge-ai\context-fusion\artifacts\real-audio-evaluation.json"
```

운영 임계값은 같은 원본 녹음이 양쪽에 섞이지 않은 보정/독립 확인 집합으로 정한다.
노션 CTX-003 기준에 따라 조용 모드 오탐률은 반드시 5% 미만이어야 한다. 아래 명령은
임계값과 활성·해제 유지 횟수를 비교하고, 합격 여부가 포함된 정책 파일을 만든다.

```powershell
& .\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe `
  tune-audio-policy `
  --manifest .\services\edge-ai\audio-context\data\public-balanced-v6-split.csv `
  --activity-model .\services\edge-ai\audio-context\artifacts\public-balanced-v6-ensemble-50-50\activity.joblib `
  --sound-model .\services\edge-ai\audio-context\artifacts\public-balanced-v5-window4s\sound_event.joblib `
  --calibration-split validation `
  --holdout-split test `
  --calibration-per-label 75 `
  --holdout-per-label 75
```

v6.2.1은 보수형과 고재현율형 활동 분류기의 확률을 50:50으로 합친다. YAMNet 특징은
한 번만 계산하므로 두 번째 경량 분류기의 추가 비용은 작다. 원본 그룹이 겹치지 않는
검증 375개와 test 375개에서 모두 노션 CTX-003 정책 게이트를 통과했다. test 결과는
정확도 88.0%, 조용 모드 재현율 91.33%, 오탐률 4.89%, 활성 지연 p95 7초,
해제 지연 대용치 p95 5초다. 정책은 `config/audio-policy-v6-ensemble-50-50.json`에
저장된다. 해제 지연은 무음 꼬리를 붙인 대용치이므로 실제 Pi 5·대회용 마이크의
전환 녹음으로 마지막에 재확인한다.

### v6.3 해제 유지 정책 (기본값, 2026-10-22)

v6.2.1은 실제 회의 음성 30분(6분 회의 / 2분 조용, `tools/flap_check.py`)에서 말 사이
침묵마다 해제돼 조용 모드가 40번 켜졌다 꺼졌다. v6.3은 같은 분류기·같은 켜짐 조건(3회)에
해제만 15회 연속으로 늘려 4번(이상값 4)으로 줄였다. 같은 검증·test 375개씩에서 재현율
91.33%, 오탐률 4.89%, 활성 p95 7초는 그대로이고 해제 지연 대용치 p95는 18초다. 해제 지연
기준은 시연용으로 8초 → 20초로 완화했다(바로 끄려면 "회의 끝났어"). v6.2.1 파일은 되돌리기용으로
남긴다. 해제 대용치의 무음 꼬리는 이제 `release_updates + 4`창(최소 6)이라 긴 유지 정책도 잰다.

```powershell
.\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe `
  tune-audio-policy `
  --manifest .\services\edge-ai\audio-context\data\public-balanced-v6-split.csv `
  --activity-model .\services\edge-ai\audio-context\artifacts\public-balanced-v6-ensemble-50-50\activity.joblib `
  --sound-model .\services\edge-ai\audio-context\artifacts\public-balanced-v5-window4s\sound_event.joblib `
  --calibration-split validation --holdout-split test `
  --calibration-per-label 75 --holdout-per-label 75 `
  --quiet-thresholds 0.5 --meeting-tv-margins 0.0 --activation-updates 3 --release-updates 15 `
  --max-release-p95 20 --config-version audio-policy-v6.3.0-ensemble-50-50-hold15 `
  --report .\services\edge-ai\context-fusion\artifacts\audio-policy-tuning-v6.3-hold15.json `
  --config-out .\services\edge-ai\context-fusion\config\audio-policy-v6.3-hold15.json
```

## Pi 5 릴리스 후보

`package-release`는 모델, 정책, 성능표, 실행 wheel과 v5 롤백 모델을 복사하고 모든
파일의 SHA-256과 크기를 `package-manifest.json`에 기록한다. 원본 음성·영상과
전사문은 포함하지 않는다. 성능 기준 미통과 또는 서명 전인 묶음은
`STAGING_ONLY`이며 활성화할 수 없다.

```powershell
homemind-context-fusion verify-release `
  .\services\edge-ai\context-fusion\artifacts\deploy\audio-context-v6.2.1-ensemble-staging
```

v6.2.1 스테이징은 10개 파일의 SHA-256 검증을 통과했다. 성능 게이트는 모두
통과했지만 전자서명이 없으므로 `STAGING_ONLY`이며 자동 활성화할 수 없다.

## 다음 연결점

1. `audio-context` 출력에서 오디오 문맥과 신뢰도를 받는다.
2. VAD·턴테이킹 모듈의 요약 특징을 받는다.
3. `vision-presence`의 안정화된 구역과 신뢰도를 받는다.
4. 결과를 MQTT `pfn/v1/context/activity`로 Pi 4에 전달한다.

## 비동기 특징 스트림

각 모델의 실행 주기가 달라도 `contracts/rpi5-internal/context-feature-observation.schema.json`의
이벤트로 연결할 수 있다. 오디오 창이 융합 시계 역할을 하며, 오디오 이벤트가 올 때
아래 최신 보조 입력을 합친다.

| 이벤트 | 실제 공급자 | 포함 정보 |
|---|---|---|
| `AUDIO_CONTEXT` | `pi5.audio-context` | 맥락, 신뢰도, VAD 발화 비율, 지속 시간 |
| `TURN_TAKING` | `pi5.turn-taking` | 분당 발화 교대, 신뢰도, 측정 방식 |
| `PRESENCE` | `pi5.vision-presence` | 신원 없는 모형 구역과 신뢰도 |
| `DEVICE_STATE` | `pi4.device-state` | TV·노트북·헤드폰 상태 스냅샷 |

마이크는 방향 정보가 없는 USB 마이크 1개이므로 음원 방향 특징은 없다. 각 이벤트의
`source_node_id`는 `pi5.*`이면 `NODE-RPI5-02`, `pi4.device-state`이면
`NODE-RPI4-01`, `pc.scenario`이면 `PC-SIMULATION`(`simulated=true`)이다.

각 이벤트에는 `timestamp`와 `valid_until`이 필수다. 만료·미도착·미래 시점·낮은
신뢰도 입력은 `UNKNOWN`으로 처리하고, 같은 `event_id`가 재전송되면 한 번만
처리한다. `SPEECH_BURST_PROXY`는 실제 화자 교대가 아니므로
`SPEAKER_CHANGE`보다 낮은 가중치를 사용한다.

하드웨어 없는 PC 합성 시나리오는 다음 명령으로 확인한다.

```powershell
& .\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe `
  stream-replay `
  .\services\edge-ai\context-fusion\scenarios\async_features.jsonl `
  --events-jsonl .\services\edge-ai\context-fusion\artifacts\async-feature-events.jsonl `
  --debug-jsonl .\services\edge-ai\context-fusion\artifacts\async-feature-debug.jsonl
```

이 시나리오는 회의 3회 연속 관측 뒤 `QUIET_REQUIRED` 활성화, 회의 종료 뒤 해제,
TV 상태를 이용한 회의 오탐 차단을 순서대로 검사한다. 모든 입력이
`simulated=true`이고 출력 근거에도 이 사실이 표시되므로 실제 장비 성능 수치와
혼동하지 않는다.

기존 오디오 서비스의 마이크 입력을 표준 입력으로 바로 받을 수도 있다.

```powershell
& .\services\edge-ai\audio-context\.venv\Scripts\homemind-audio.exe live `
  --activity-model .\services\edge-ai\audio-context\artifacts\public-balanced-v4-temporal\activity.joblib `
  --sound-model .\services\edge-ai\audio-context\artifacts\public-balanced-v4-temporal\sound_event.joblib `
  --feature-events |
& .\services\edge-ai\audio-context\.venv\Scripts\homemind-context-fusion.exe `
  stream-replay -
```

이전 평면 JSON과 `audio`, `vision`, `features`, `device_state` 중첩 객체는 단일
관측 재생에 계속 사용할 수 있다. 실제 서비스 연결에는 위 비동기 이벤트 계약을
사용한다.

현재 시나리오는 실제 마이크·카메라가 없는 상태에서 계약과 상태 전이 로직을
검증하기 위한 합성 입력이다. 실제 정확도 평가는 모형 공간과 대회용 카메라·마이크가
준비된 뒤 별도의 녹화/녹음 데이터로 수행해야 한다.
