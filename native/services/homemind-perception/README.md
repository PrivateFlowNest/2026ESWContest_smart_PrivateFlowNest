# homemind-perception — NODE-RPI5-02 인식 서비스 (C++ 네이티브)

`services/runtime/src/homemind_runtime/perception.py`(PerceptionService)를 C++20으로 옮긴 실행 파일.
RPI5(LG AI Native OS, Yocto, glibc 2.39, aarch64)에는 Python·Docker가 없다. Python 쪽은 그대로 두고 대조 기준(oracle)으로 쓴다.

> 이 문서의 수치는 모두 **PC x86_64 Docker**(또는 Windows Python) 결과다. 라즈베리파이 실기 결과가 아니다.
> 마이크 경로는 ALSA `file` 플러그인으로만 시험했다(실제 마이크 아님).

```
ALSA 마이크 (16 kHz mono S16, 100 ms 블록) ─┐            WAV 파일 (--wav, 실시간 또는 --fast)
                                             ▼                         │
                              AudioRing (소비자별 커서, 30 s)  ◀────────┘
 ├─ 상황 경로: 4 s 창 / 1 s 간격 → libs/hm_context (YAMNet ONNX + VAD 특징 + activity 헤드 + context-fusion)
 │     → pfn/v1/ai/perception ACTIVITY_CONTEXT : 전이(ACTIVATED/UPDATED/DEACTIVATED)마다 + 10 s마다 UNCHANGED
 └─ 음성 경로: 에너지 VAD 끝점(libs/hm_audio, vad.py와 같은 결정) → libs/hm_stt (whisper-server HTTP, whisper-cli 예비)
       → libs/hm_parser VoiceParser → pfn/v1/core/task/request 또는 pfn/v1/ai/llm/request
          (선택) 잡는 동안 libs/hm_vosk partial → libs/hm_partial 게이트 → PARTIAL_INTENT_HINT (아래 "부분 인텐트 힌트")
 누름 통화: stdin · FIFO(ptt_fifo) · MQTT pfn/v1/ai/voice/listen(7인치 UI)
 카메라 재실: homemind-vision → FIFO(presence_fifo) → context-fusion (아래 "카메라 재실")
```

## 폴더 / 타깃

| 경로 | 내용 |
|---|---|
| `native/libs/hm_audio` | WAV 읽기·쓰기(PCM 8/16/24/32, float 32/64, 다채널→평균), scipy `resample_poly` 호환 리샘플러(일괄 + 스트리밍), 에너지 VAD(vad.py 포팅), 링버퍼 fan-out, ALSA 캡처, 누름 통화 트리거(stdin, FIFO). 타깃 `hm_audio`, `hm_audio_probe`, `hm_audio_tests` |
| `native/libs/hm_stt` | Transcriber: whisper-server `/inference` 우선, whisper-cli 예비, `clean_transcript`. 타깃 `hm_stt`, `hm_stt_probe`(PC 시험용), `hm_stt_tests` |
| `native/libs/hm_vosk` | Vosk 0.3.45 C API를 실행 때 `dlopen` (빌드에 Vosk 불필요), Python `VoskBackend`와 같은 partial 수집(`PartialStream`). 타깃 `hm_vosk`, `hm_vosk_probe`(PC 시험용), `hm_vosk_tests`. 시험용 libvosk는 configure 때 `native/.deps/downloads/vosk-0.3.45/vosk-linux-<arch>-0.3.45.zip`을 빌드 폴더(`/build/_deps`)에 푼다 |
| `native/services/homemind-perception` | `homemind-perception`(배포 대상), `homemind-perception-harness`(PC 시험용, 대역(doubles) 포함, 배포 금지), `homemind-perception-probe`(voice/listen 스키마 판정, PC 시험용), `homemind_perception_tests` |
| `native/tests/test_perception_*.py` | Python 대조·통합 시험 (아래) |

`hm_context`(agent C), `hm_parser`(agent A) 타깃이 있으면 링크하고(`HM_PERCEPTION_WITH_CONTEXT/PARSER`, 기본 ON),
없으면 빌드는 되고 시작할 때 "context path disabled / voice path disabled"를 로그로 남긴다.

## 빌드

```bash
# PC (x86_64 Docker, 이미지 homemind-native-dev:24.04). 저장소 루트에서:
docker run --rm -v "$PWD:/w" -v hm-native-build-perception:/build homemind-native-dev:24.04 bash -c \
  "cmake -S /w/native -B /build -G Ninja -DCMAKE_BUILD_TYPE=Release && cmake --build /build --target homemind-perception"
```
aarch64(IPK)는 다른 네이티브 서비스와 같은 방식(Ubuntu 24.04 arm64, buildx)으로 만든다. 이 작업에서는 aarch64 빌드를
돌리지 못했다(Docker VM에 QEMU binfmt가 등록돼 있지 않음). 코드는 x86 전용 명령을 쓰지 않고, VAD·리샘플러는
`-ffp-contract=off`로 aarch64에서도 NumPy/scipy와 같은 산술이 되게 했다.

## 실행

```bash
homemind-perception --list-devices                       # ALSA 캡처 장치 (* = default)
homemind-perception --config /etc/homemind/perception.json   # 마이크 모드 (Ctrl+C / SIGTERM 종료)
homemind-perception --config cfg.json --wav a.wav [--wav b.wav] --fast --stdout   # 파일 모드
#   --voice auto|whole|vad|off  --offset S  --seconds S  --listen-mode push_to_talk|always|off
#   --presence-zone ZONE  --device NAME  --debug-transcripts  --ptt-fifo PATH  --log-level DEBUG|INFO|WARNING|ERROR
#   --presence-fifo PATH  --partial-intent  --voice ptt (native: 실시간 파일 재생 + 누름 통화, PC 시험용)
```
- 명령행·설정 키·기본값은 Python `python -m homemind_runtime.perception`과 같다(+ 아래 native 전용 키).
- `--stdout`: `토픽 JSON` 줄. 콘솔 안내(`[voice] ...`, `Enter를 누르고 말하세요` 등)도 Python처럼 stdout에 나온다.
- 파일 모드는 끝나면 stderr 마지막 줄에 요약 JSON(`windows`, `transitions`, `voice`)을 쓴다(Python과 같은 키).
- 파일 모드 결과는 항상 `simulated: true`(perception, llm/request). 마이크 모드는 설정 `simulated`를 따른다.
- 종료 코드: 0 정상, 1 설정/시작 오류(설정 파일 없음, 모델 파일 없음 등), 2 명령행 오류.

## 설정 키 (설정 JSON 최상위; Python DEFAULTS와 같은 이름·기본값)

상대 경로는 `RuntimeConfig::resolve_path` 기준(PC 빌드: 저장소 루트, 장치: 설정 파일 폴더 또는 `base_dir`). `--wav`는 현재 폴더 기준.

| 키 | 기본값 | 설명 |
|---|---|---|
| `audio_device` | `null` | null/"" = ALSA `default`, 숫자 N = `plughw:N`(카드 번호), `:` 포함 = PCM 이름 그대로, 그 밖 = 장치 이름·설명 부분 일치(대소문자 무시) |
| `listen_mode` | `push_to_talk` | `push_to_talk` / `always` / `off`(음성 경로 끔). 상황 경로는 항상 동작 |
| `presence_zone`, `presence_confidence` | `UNKNOWN`, 0.9 | 카메라 없음 → UNKNOWN. 값을 주면 fusion에 `simulated` PRESENCE 입력 |
| `fusion_policy` | `services/edge-ai/context-fusion/config/audio-policy-v6.3-hold15.json` | v6.3: 해제 15회 유지(회의 중 깜빡임 방지). 되돌리기 v6.2.1 `audio-policy-v6-ensemble-50-50.json` |
| `window_seconds`, `hop_seconds` | 4.0, 1.0 | |
| `republish_seconds`, `perception_ttl_seconds` | 10.0, 15.0 | 재발행 주기(오디오 시간), `valid_until_ms` = 발행 시각 + 15 s, 파서에 주는 context도 15 s 지나면 버림 |
| `vad` | `{}` | `frame_ms 20, start_ms 200, start_ratio 0.8, end_silence_ms 700, max_ms 8000, min_ms 400, pre_roll_ms 300, tail_ms 200, threshold_db -45, noise_margin_db 10, noise_adapt 0.05`. `*_ms`는 정수만. 잘못된 값은 시작 때 오류 |
| `ptt_timeout_seconds` | 6.0 | 누른 뒤 이 시간 안에 말이 시작되지 않으면 취소 |
| `whisper_server_url` | `null` | 예: `http://127.0.0.1:8178`. 있으면 우선 사용 |
| `whisper_cli_path`, `whisper_model`, `whisper_threads` | `native/.deps/x64-tools/bin/whisper-cli`, lora-v4 q5_0, 8 | CLI 인자 `-m -f -l ko -t -np -nt -ng` (Python과 같음). Python 기본 경로(.exe)는 Linux에서 쓸 수 없어 기본값만 다름 |
| `stt_model_version` | `whisper-small-homemind-lora-v4-q5_0` | 비우면 `whisper.cpp-<모델 파일 이름>` |
| `debug_transcripts` | `false` | 리허설 전용. 켜야만 전사문을 로그·콘솔에 보임 |
| `publish` | `mqtt` | `mqtt` / `stdout` |
| `wav_input`, `wav_fast`, `wav_voice`, `wav_offset_seconds`, `wav_max_seconds`, `max_whole_file_seconds` | null, false, auto, 0, null, 30 | 파일 모드. `auto` = 30 s 이하면 `whole`(파일 전체 = 발화 1개), 넘으면 `off` |
| `activity_model`, `yamnet_model` | (Python 경로) | Python 전용. C++는 읽지 않지만 같은 설정 파일을 쓰도록 키는 받음 |
| **native 전용** | | |
| `yamnet_onnx` | `native/.deps/models/yamnet/yamnet-embeddings.onnx` | agent C가 변환한 YAMNet (hm_context README) |
| `activity_head_json` | `native/.deps/models/activity-head/public-balanced-v6-ensemble-50-50.json` | agent C가 내보낸 activity 헤드 |
| `device_capabilities` | `services/exaone-edge/config/device-capabilities.json` | VoiceParser 입력 (Python은 패키지 기본 경로) |
| `context_threads` | 2 | ONNX Runtime intra-op 스레드 |
| `stt_timeout_seconds` | 60.0 | whisper-server 요청·whisper-cli 실행 제한 시간(넘으면 CLI는 kill) |
| `ptt_fifo` | `null` | 누름 통화 FIFO 경로(아래). `--ptt-fifo`로도 지정 |
| `presence_fifo` | `null` | 카메라 재실 FIFO(homemind-vision `vision.presence_fifo`와 같은 경로, 기기 `/run/homemind/presence`). 주면 `presence_zone` 고정값은 무시(경고). 아래 "카메라 재실" |
| `partial_intent` | `{"enabled": false, ...}` | 부분 인텐트 힌트(Vosk + partial-intent). 아래 "부분 인텐트 힌트" |
| `wav_voice` = `ptt` | | native 전용: 실시간 파일 + 누름 통화(stdin·FIFO·MQTT 시험용, `--fast` 무시) |

MQTT는 `broker` 블록(host, port, username, `password_file`, keepalive)과 `perception_node_id`, `llm_request_ttl_ms`,
`simulated`, `contracts_dir`를 `hm::RuntimeConfig`로 읽는다. 비밀번호는 파일 경로로만 받는다(`broker.password`는 거부).
클라이언트 id는 `<perception_node_id>-perception`.

## 누름 통화 (push-to-talk)

- 콘솔: Python처럼 Enter 한 줄 = 한 번 누름(대기 중이면 듣기 시작, 듣는 중이면 바로 끝냄).
- **FIFO (Pi, 터미널 없이)**: `ptt_fifo`를 주면 시작할 때 FIFO를 만든다(모드 0660, 상위 폴더는 미리 있어야 함, 같은 경로에 일반 파일이 있으면 거부).
  한 줄 = 명령 하나:
  - `press` 또는 빈 줄 → Enter와 같음
  - `start` → 듣기 시작(이미 듣는 중이면 무시)
  - `stop` → 바로 끝냄(대기 중이면 무시)
  - `cancel` → 바로 멈추고 녹음을 버림(whisper로 보내지 않음, 대기 중이면 무시)

  ```bash
  # systemd 예: RuntimeDirectory=homemind  (→ /run/homemind), 설정 "ptt_fifo": "/run/homemind/ptt"
  echo press > /run/homemind/ptt      # 버튼 스크립트, 같은 Pi의 UI 등
  ```
  서비스가 FIFO를 읽기·쓰기로 열어 두므로 쓰는 쪽이 열고 닫아도 끊기지 않는다. 서비스가 꺼져 있으면 `echo`가 막히니 버튼 스크립트는 `timeout 1 sh -c 'echo press > /run/homemind/ptt'`처럼 쓴다.
- 다른 노드(UI가 있는 RPI5-01)에서 누르기: MQTT `pfn/v1/ai/voice/listen` (아래).

## 7인치 UI 누름 통화 (`pfn/v1/ai/voice/listen`, 2026-10-22 agent F)

계약: `contracts/pfn.v1/voice-listen.schema.json`, `protocol.json` `ai_voice_listen`(RPI5-01 발행 → RPI5-02 구독, QoS 1, core 구독 안 함).
`listen_mode`가 `push_to_talk`일 때만 구독한다(파일 시험은 `--voice ptt`). 처리 순서:

1. 스키마 검사(손으로 구현, uuid 형식은 jsonschema FormatChecker와 같은 `hm_guard` 판정) → 실패: `ignored ... invalid (<필드>: <규칙>)`
2. `target_node_id` = 이 노드의 `perception_node_id` → 아니면 `addressed to another node`
3. 지금 > `expires_at_ms` → `expired N ms ago` (UI는 +3 s로 보냄. **두 Pi5 시계가 3초 넘게 어긋나면 모든 누름이 버려진다 → P9에서 시간 동기 확인**)
4. 같은 `event_id`(최근 256개) → `duplicate event` (QoS 1 재전송)
5. 누름 통화 제어가 없음(음성 경로 꺼짐 등) → `push-to-talk is not active`

`start` = FIFO의 `start`와 같음(듣는 중이면 무시). `cancel` = 듣기를 바로 멈추고 **녹음을 버린다**(whisper로 보내지 않음, 콘솔
`듣기를 취소했습니다`; 대기 중이면 무시). FIFO·stdin도 `cancel` 줄을 받는다. 무시한 요청의 내용(client_id, action 값 등)은 로그에 쓰지 않는다.
원격 `start`의 `trace_id`(소문자 uuid일 때)가 그 발화의 trace가 된다: 부분 인텐트 힌트와 최종 task/llm 요청이 UI 누름과 같은 trace를 쓴다.

## 카메라 재실 → context-fusion (`presence_fifo`)

homemind-vision이 `presence_cameras`(기본 CAM-INDOOR-01)의 안정 zone이 바뀔 때·처음·10 s마다 rpi5-internal `PRESENCE` 한 줄을
FIFO에 쓴다. homemind-perception은 같은 경로를 읽어(없으면 0660으로 만듦, FIFO가 아닌 파일이면 거부, 읽기/쓰기로 열어 EOF 없음)
`hm_context` `submit_presence`로 넘긴다. 줄 하나 = JSON 하나(최대 8 KiB). JSON 아님·스키마 위반·다른 event_type은 세고 내용 없이 로그(처음 20개, 이후 1000개마다).

- `presence_fifo`를 주면 `presence_zone` 고정값은 쓰지 않는다(경고 로그 후 UNKNOWN). 둘이 함께 들어가지 않는다
- 재실은 오디오 창 시계에 맞춰 넣는다: 창 timestamp 이하인 것만 그 창 직전에(hm_context README). vision과 perception은 같은 Pi의 시계를 쓴다
- vision이 꺼져 있으면 재실이 15 s(`presence_ttl_seconds`) 뒤 stale → zone UNKNOWN (fusion 규칙 그대로)
- perception이 꺼져 있으면 vision 쓰기는 버려진다(읽는 쪽 없음). 다시 켜지면 다음 재발행(≤10 s)부터 이어진다

기기 경로와 systemd (패키징에서 반영할 것, 이 폴더에서는 바꾸지 않음):

```ini
# homemind-perception.service, homemind-vision.service 둘 다
RuntimeDirectory=homemind
RuntimeDirectoryMode=0770
RuntimeDirectoryPreserve=yes   # 한 유닛이 재시작해도 /run/homemind(FIFO)가 지워지지 않게. 없으면 vision의 마운트 네임스페이스가 지워진 폴더를 계속 봄
# 설정: perception "presence_fifo": "/run/homemind/presence", vision "vision": {"presence_fifo": "/run/homemind/presence"}
#       "ptt_fifo": "/run/homemind/ptt" (기존)
```
vision 유닛은 지금 `ProtectSystem=strict`에 `RuntimeDirectory`가 없어 `/run/homemind`에 쓸 수 없다. 두 유닛 모두 `User=network`.

## 부분 인텐트 힌트 (`partial_intent`, 기본 꺼짐)

아키텍처 1·3-1: partial 문장은 읽기 전용 준비 힌트, 실행은 Whisper 최종 문장 뒤에만, `execution_allowed`는 항상 false.

```
음성 경로가 발화를 잡는 동안 (VAD 시작·누름 통화·whole 파일)
  열린 발화의 오디오(pre-roll 포함) ─▶ 작업 스레드: Vosk(libs/hm_vosk, 100 ms 조각) ─▶ 새 partial 문장
     ─▶ 발화마다 새 StreamIntentGate(libs/hm_partial, 같은 의도 DETECTED 2회) ─▶ PARTIAL_INTENT_HINT (pfn/v1/ai/perception)
  발화 끝 ─▶ whisper 최종 → 파서 → task/llm 요청 (힌트와 같은 trace_id)
```
- Vosk 최종 결과는 쓰지 않는다(Python cascade와 같음). partial 문장은 로그에 남지 않는다(`debug_transcripts`일 때만 `partial (debug_transcripts) trace=...`)
- 마이크: Vosk가 실시간보다 느리면 음성 경로를 막지 않는다. 발화가 끝나면 아직 못 읽은 오디오를 버리고, 대기 오디오가
  `max_queue_seconds`를 넘은 발화는 더 이상 힌트를 내지 않는다(로그 1회). 파일 모드: 전부 디코딩(결정적, Python과 같은 조각)
- libvosk.so는 `dlopen`으로 연다. 켜지 않으면 필요 없다. 켰는데 libvosk·모델·ONNX가 없으면 힌트만 꺼지고(오류 로그) 나머지는 정상
- VAD가 너무 짧다고 버린 발화도 잡는 동안 낸 힌트는 이미 나갔을 수 있다(읽기 전용 힌트라 실행 영향 없음)

| 키 (`partial_intent.*`) | 기본값 | 설명 |
|---|---|---|
| `enabled` | `false` | `--partial-intent`로도 켬 |
| `vosk_library` | `libvosk.so` | `/` 없으면 동적 로더 검색(LD_LIBRARY_PATH), 있으면 경로(base_dir 기준) |
| `vosk_model` | `services/edge-ai/speech-to-text/models/vosk-model-small-ko-0.22` | 풀어 둔 모델 폴더 (253 MB) |
| `onnx_model` | `services/edge-ai/partial-intent/artifacts/partial-intent-v2.onnx` | `.json` 메타데이터가 옆에 있어야 함 |
| `chunk_ms` | 100 | Vosk 조각 (Python `VoskBackend(chunk_ms=...)`, 기본 250과 다름에 주의) |
| `confirmation_updates` | 2 | StreamIntentGate |
| `max_queue_seconds` | 3.0 | 마이크 모드 대기 한도 |


## 시험 추가분 (agent F, 2026-10-22, PC x86_64 Docker — Pi 실기 아님)

```bash
export HM_NATIVE_BUILDER=perception2 PYTHONUTF8=1
services/runtime/.venv/Scripts/python -m pytest native/tests/test_perception_live.py native/tests/test_perception_partial.py \
  -q -s -p no:cacheprovider --basetemp "$TEMP/hmpt/f1"
```

| 시험 | 결과 |
|---|---|
| gtest `homemind_perception_tests` / `hm_vosk_tests` / `hm_audio_tests` | 28 / 6 / 18 통과 (전체 13묶음 ctest 통과, Debug·Release `-Werror` 경고 0) |
| voice/listen 스키마: C++ 판정 vs Python jsonschema(Draft 2020-12 + FormatChecker), 문서 106개(유효 35, uuid 변형 30개 포함) | **불일치 0** |
| voice/listen MQTT(컨테이너 mosquitto, `--voice ptt` 14초 실시간): 만료·잘못된 action·같은 event_id 재전송·start·cancel | 만료/무효/중복은 무시 로그(값 인용 없음), start 2회·cancel 1회, task request 1개(첫 누름만, trace = UI 누름 trace), cancel한 발화는 whisper로 안 감 |
| voice/listen, `listen_mode`가 push_to_talk가 아닐 때 | 구독 안 함(로그 0) |
| 재실 스크립트(11개: zone 변경, 신뢰도 0.3, 같은 event_id, 5초 과거, 0.7초 미래, TTL 3초, 스키마 위반) + 실제 YAMNet/fusion, AIHub 회의 75초 | 72창 **fused event 72/72 Python `FeatureStreamFusion`과 동일**(event_id 제외), 넣기 결과 buffered 8·duplicate 1·out_of_order 1, 상태 fresh 52·low_confidence 8·stale 12, `future` 0, 무효 1개는 거부 |
| homemind-vision(COCO 후보, dog.jpg 5 fps, zone 전체=DESK) → `/tmp/hm/presence` → homemind-perception(AIHub 회의 24초 실시간) | FIGURE_PRESENCE 실내 3·현관 3, FIFO 수신 3(현관 0 = `presence_cameras`), ACTIVITY_CONTEXT 4개 전부 zone `desk`, 21창 Python 재생 21/21 동일. 17초에 DESK+QUIET → STUDYING(quiet) UPDATED (재실이 fusion 결과를 바꿈) |
| Vosk partial: C++ hm_vosk vs Python `VoskBackend` 알고리즘(같은 libvosk x86_64, ctypes), AIHub 71405 60개, 100 ms | **partial 열 60/60 동일**(386개 문장), Vosk 최종 문장 59/60(서비스는 안 씀) |
| 위 ctypes 이식 vs 진짜 `VoskBackend` (Windows, 같은 libvosk.dll) | 15/15 동일 |
| C++ vs Windows Python 그대로(vosk 휠의 libvosk.dll) | partial 열 41/60 동일, C++ 문장 386개 중 324개가 Windows 열에도 있음, 최종 46/60 (빌드마다 부동소수점 차이) |
| libvosk 상태 의존: 같은 20개를 한 프로세스에서 두 번 | 두 번째 partial 12/20만 같음. C++·Python이 두 번 모두 같음 → libvosk 성질 |
| 서비스 whole 모드(클립마다 새 프로세스, 실제 Vosk + partial-intent ONNX) 21개 | partial = 새 프로세스 probe와 21/21 동일, 힌트 16개 = Python `StreamIntentGate`(발화마다 새 게이트)와 동일, task request trace = 힌트 trace |
| 서비스 VAD 모드, 30개 연결 스트림 | 발화 31·음성 결정 31·partial 194·힌트 10, Python 게이트와 동일, 힌트 trace ⊂ 결정 trace, 힌트에 원문 없음·`execution_allowed` false·스키마 통과 |
| arm64 빌드 (QEMU, 패키징과 같은 Release `-mcpu=cortex-a76` `-Werror`, 전체 트리) | 경고 0, ctest hm_common·hm_audio·hm_context·hm_partial·hm_vision·hm_vosk·homemind_perception 7묶음 통과 |
| QEMU arm64 (aarch64 libvosk 0.3.45, 에뮬레이션) | 로드·실행 됨: `hm_vosk_tests` 6/6, 5개 클립 partial **x86_64와 5/5 동일**. 의존 `libatomic.so.1`·libstdc++·libm·libgcc_s·libc, 요구 GLIBC ≤ 2.17, GLIBCXX ≤ 3.4.18 (glibc 2.39와 충돌 없음) |

지연·메모리 (PC x86_64 Docker Release, Ryzen 9 5900X, Pi 수치 아님. 다른 에이전트 컨테이너가 동시에 돌던 상태):

| 항목 | 값 |
|---|---|
| Vosk 100 ms 조각당 (accept + partial/result + JSON), AIHub 30개 86.9 s, 884조각 | p50 **0.34 ms**, p95 **275 ms**, p99 835 ms, 최대 950 ms, 평균 60.7 ms → **RTF 0.68** (Kaldi가 몇 조각마다 몰아서 디코딩) |
| 서비스 작업 스레드(100 ms 링 블록당, VAD 스트림 815블록) | p50 0.43 ms, p95 324 ms, 최대 885 ms. 게이트(ONNX+정규화) partial당 p50 0.16 ms, 최대 0.68 ms |
| 발화당 recognizer 생성 | p50 68 ms |
| 모델 로드 / 메모리 | 6.4 s(저장소 폴더 bind mount, 부하 상태; 다른 실행 3.4–5.3 s), RSS 모델 후 503 MB, 디코딩 중 최대 567 MB |
| 참고: Windows Python `VoskBackend` 60개 | RTF 0.84 |
| QEMU arm64 (에뮬레이션, Pi 수치 아님) | 모델 로드 42.5 s, RTF 16.1, RSS 520 MB |

Pi 5는 PC보다 단일 스레드가 2~3배 느리다고 보면 RTF가 1을 넘을 수 있다(실시간보다 느림). 그 경우 발화 중 힌트가 늦어지고
`max_queue_seconds`(3 s)를 넘는 발화는 힌트를 포기한다(음성 경로는 영향 없음). **Pi5 실측(P9) 전에는 켜지 않는 것을 권장.**

## 장애 때 동작

- **whisper-server 꺼짐**: 그 발화만 `whisper failed for a N.Ns utterance` 오류 로그를 남기고 버린다. 상황 경로·MQTT는 계속. 서버가 다시 뜨면 다음 발화부터 정상
  (시험: 서버를 실행 중 6 s에 띄움 → 첫 발화 실패, 둘째 발화 정상). `whisper_cli_path`+모델이 있으면 서버 실패 때 CLI로 한 번 더 시도.
- **MQTT 끊김**: `hm::Bus`(libmosquitto)는 끊긴 동안의 QoS1 발행을 거부한다(`MOSQ_ERR_NO_CONN`). Python(paho)은 쌓아 뒀다가 보낸다. 그래서
  `BusPublisher`가 보관함(최대 256개)에 두었다가 재연결 뒤 순서대로 보낸다: perception은 `valid_until_ms`까지, llm/request는 `expires_at_ms`까지,
  core task/request는 10 s까지. 지난 것은 버리고 로그를 남긴다(오래된 장치 명령을 늦게 보내지 않게). 재연결은 1→30 s 간격(hm_common).
  시험: 실행 중 브로커를 3 s에 끄고 7 s에 다시 켬 → 6개 모두 도착(잃은 것 없음).
- **마이크 없음/빠짐**: 캡처 스레드가 오류 로그를 남기고 2 s마다 다시 연다(Python은 시작 때 실패). 다른 경로는 대기.
- 한 창·한 발화의 예외는 로그만 남기고 계속(Python과 같음).

## 개인정보

- 원음은 저장하지 않는다. whisper-server로는 메모리에서 만든 WAV를 바로 보낸다(임시 파일 없음). whisper-cli를 쓸 때만
  `$TMPDIR/hm-stt-XXXXXX/utterance.wav`(폴더 0700, 파일 0600)를 만들고 CLI가 끝나면(실패·시간 초과 포함) 바로 지운다.
- 전사문은 `debug_transcripts`일 때만 로그·콘솔에 나온다. perception에는 결과만 들어간다. 전사문은 파서가 못 푼 경우 `ai/llm/request`로만 나간다(계약).

## Python과 다르게 한 것 (의도적, 결과 메시지는 같음)

1. **파일 모드 context 결정성**: 음성 경로가 발화 끝 시각까지 상황 경로가 따라오기를 기다린 뒤 "발화 끝 이전의 마지막 창" context를 쓴다.
   Python `--fast`는 두 스레드가 경주해서 VAD 모드의 llm/request `context`가 실행마다 달라질 수 있다. 마이크 모드는 Python과 같이 "가장 최근" context.
2. 마이크가 16 kHz를 못 받을 때: 연속 스트리밍 리샘플러(전체 신호에 `resample_poly`를 한 것과 같은 값). Python은 100 ms 조각마다 따로 리샘플(조각 경계 왜곡).
3. `vad` 설정을 시작할 때 검사(잘못되면 시작 실패). Python은 음성 스레드 안에서 검사해서 음성 경로만 조용히 죽는다.
4. 소비자가 밀려 오디오가 빠지면 상황 창을 새로 시작(Python은 끊긴 오디오를 이어 붙임).
5. 음성 경로가 꺼져 있으면(whisper 없음) 누름 통화 안내·stdin 읽기를 하지 않는다.
6. MQTT 보관함(위), 마이크 재시도(위), whisper-cli 시간 제한(Python은 무제한).
7. FLAC 입력은 지원하지 않는다(장치에 libsndfile 없음). WAV만.

## Python 쪽에서 찾은 문제 (Python 코드는 고치지 않음, parity는 유지)

- (agent F) `VoskBackend` 기본 `chunk_ms`는 250, 서비스는 100 ms 블록 → 비교하려면 `chunk_ms=100`을 줘야 한다. 또 엔드포인트(`Result()`) 뒤에
  `last_partial`을 초기화하지 않아, 다음 구간의 첫 partial이 직전 partial과 같으면 빠진다(C++도 같은 동작 유지)
- (agent F) libvosk 0.3.45 + 이 한국어 모델은 상태가 남는다: 같은 프로세스에서 같은 음성을 다시 디코딩하면 partial이 달라질 수 있다
  (20개 중 8개). Python cascade도 같다. 결과 재현에는 같은 디코딩 이력이 필요하다
- (agent F) Python 런타임(perception.py)에는 실시간 카메라 재실 입력도 partial 경로도 없다(고정 `presence_zone`만). cascade는 파일 전체를
  Vosk로 다 읽은 뒤 partial을 내므로 실시간 힌트가 아니다

- `--fast` VAD 파일 모드에서 llm/request `context`가 스레드 타이밍에 따라 달라짐(위 1)
- `MicrophoneSource._pump`: 100 ms 조각별 `resample_poly`(위 2)
- `EndpointConfig.from_mapping`이 음성 스레드 안에서 실행되어 잘못된 `vad` 설정이 음성 경로만 죽임. `*_ms`에 실수(예 `200.0`)를 주면 `deque(maxlen=float)` TypeError
- `context_loop` 마지막 패딩 창(입력 < 4 s)은 try 밖이라 예외 하나로 상황 스레드가 끝남
- 손실 모드 fan-out에서 버려진 블록을 모르고 이어 붙임(위 4)
- `pfn.perception`은 `source_node_id`를 `NODE-RPI5-02`로 고정, 파서는 설정 `perception_node_id`를 씀(설정을 바꾸면 불일치). C++는 둘 다 설정값(기본 같음)
- paho가 끊긴 동안의 QoS1 메시지를 무제한·무기한 쌓아 재연결 뒤 오래된 장치 명령도 보냄
- `--offset/--seconds`가 여러 `--wav` 파일 각각에 적용됨(동작은 그대로 따름)

## 시험 (PC x86_64 Docker, 2026-10-22)

```bash
# 저장소 루트, Git Bash. 다른 빌더의 작업 중 폴더가 configure를 깨면 HM_PERCEPTION_ISOLATED=1(심볼릭 링크로 필요한 lib만)
export HM_NATIVE_BUILDER=perception HM_PERCEPTION_ISOLATED=1 HM_PERCEPTION_LIBS=hm_text,hm_guard,hm_parser,hm_context
services/runtime/.venv/Scripts/python -m pytest native/tests/test_perception_audio.py native/tests/test_perception_e2e.py \
  native/tests/test_perception_stt.py native/tests/test_perception_runtime.py -q -p no:cacheprovider --basetemp "$TEMP/hmpt/b1" -s
```
실제 음성: AIHub 71405(가정 근거리 명령, validation), 464(회의), 109(자유 대화), 71698(환경음 44.1 kHz)을 로컬 드라이브
(`HM_AIHUB_DIR`, 기본 `E:/HomeMind-Datasets/raw/aihub`)에서 pytest 임시 폴더로 복사해 쓰고 끝나면 지운다. 커밋하지 않는다.
x86_64 whisper: `native/.deps/x64-tools/bin/whisper-{cli,server}` (whisper.cpp b4938, `-DGGML_NATIVE=ON -DBUILD_SHARED_LIBS=OFF`, gitignore 폴더).

| 시험 | 결과 |
|---|---|
| gtest `hm_audio_tests` / `hm_stt_tests` / `homemind_perception_tests` | 17 / 7 / 14 통과 |
| VAD parity, 합성 41가지(설정 3종 × 신호 12종 + 누름 통화 5) | 9,812 프레임 voiced 불일치 0, 발화 47개 시작·끝·이유 동일, 레벨 차 최대 1.4e-14 dB |
| VAD parity, 실제 음성 642 s(명령 24개 연결, 회의 60 s, 대화, 44.1 kHz 환경음; 설정 3종 + 누름 통화) | 32,107 프레임 불일치 0, 발화 170개 동일, 레벨 차 최대 1.4e-14 dB |
| 리샘플러 vs `scipy.signal.resample_poly`(1/3, 160/441, 2/1, 320/441) | **최대 절대 차 0.0**(모든 샘플 비트 동일), 필터 계수 8,821개 중 차이 0, 스트리밍 = 일괄 |
| WAV 읽기 vs `load_wav_16k_mono`/`load_wav_segment_16k_mono`(PCM16/24/u8, float32/64, 스테레오, 8k/22.05k/44.1k/48k, 1 s 패딩, 구간) | 18가지 모두 최대 절대 차 0.0 |
| 리샘플한 입력의 VAD 결정 (각자 자기 로더) | 동일 |
| PCM16 인코딩 vs `write_pcm16_wav` | 바이트 동일 |
| 서비스 로직 parity: Python 파이프라인의 창별 fused event를 C++에 재생(가짜 파이프라인 4장면 + 실제 YAMNet/TF 회의 75 s) | perception 문서(id·시각 제외) 전부 동일(7/1/3/16/10개), 창 수·창 시각 동일 |
| **전체 네이티브(hm_context ONNX + hm_parser) vs `python -m homemind_runtime.perception --fast --stdout`**, AIHub 회의 75 s | 창 72개, perception 10/10 문서 동일, 전이 5개(7 s ACTIVATED … 72 s ACTIVATED) 동일 |
| 같은 비교 + 실제 whisper, 명령 6개 VAD 모드 | 창 23, perception 5/5, 음성 결정 6개(task_request, llm_request, clarify, llm_request, task_request, task_request)·메시지 5개 문서 동일(context 제외) |
| 같은 비교, whole 모드(명령 1개) | context 포함 전부 동일 |
| 가짜 전사 + 실제 hm_parser (VAD·whole) | 결정·토픽·문서 동일 |
| whisper-server: C++ 클라이언트 vs Python 클라이언트(같은 서버, AIHub 명령 20개) | 전사 20/20 동일, CER 9.1%, 정규화 후 완전 일치 11/20 |
| whisper-cli 예비 경로 | 서버 실패 → CLI 성공, 서버·CLI 둘 다 없음 → 오류(서비스는 계속), 임시 폴더 남은 것 0 |
| MQTT (컨테이너 안 mosquitto 2.0.18, mosquitto_sub) | 6개 수신, 경로별 순서가 stdout 실행과 같음, 전부 계약 스키마 통과 |
| 브로커 3 s에 종료 → 7 s에 재시작 | 종료 코드 0, 6/6 도착(보관 후 전송) |
| whisper-server 실행 중 기동 | 첫 발화 실패 로그, 둘째 발화 정상, 상황 경로 계속 |
| ALSA 캡처 경로(ALSA `file` 플러그인, 실제 마이크 아님) | 장치 열기·S16→float·두 소비자·발행 정상, SIGTERM 정상 종료 |
| 누름 통화 FIFO | FIFO 생성, `press` → `듣는 중...`, 1 s 무음 → 취소 안내 |
| 명령행 | `--help` 0, 잘못된 선택 2, 없는 설정 1, `--list-devices` 0 |

지연 (PC x86_64 Docker, Pi 수치 아님):

| 항목 | 값 |
|---|---|
| 상황 경로 1창 (Release, ONNX 2스레드) | p50 8 ms, p95 10 ms, 최대 12 ms; 75 s 파일 전체 0.86 s(모델 로드 포함) |
| whisper-server 발화당 (2.8 s 명령, 8스레드 CPU) | p50 2.8–3.7 s, p95 3.3–7.2 s (실행마다 PC 부하에 따라 다름), Python 클라이언트 p50 2.9–3.0 s |
| whisper-cli 발화당 (모델 매번 로드) | p50 3.5 s |

## 실행 의존성

- (agent F) `partial_intent`를 켤 때만: `libvosk.so`(dlopen, aarch64 0.3.45 7,496,288 bytes; 그 의존 `libatomic.so.1`·libstdc++·libm·libgcc_s·libc,
  GLIBC ≤ 2.17), Vosk 모델 폴더(264,364,726 bytes, 15파일), `partial-intent-v2.onnx`(147,769) + `.json`(874). 켜지 않으면 추가 의존 없음
  (`ldd homemind-perception`은 그대로: libvosk는 DT_NEEDED가 아님)

`ldd` (x86_64 Release, hm_context·hm_parser 링크): `libasound.so.2`, `libmosquitto.so.1`(→ `libssl.so.3`, `libcrypto.so.3`),
`libonnxruntime.so.1`(ORT 1.20.1, 번들 필요 → `libdl`, `librt`, `libpthread`), `libstdc++.so.6`, `libm.so.6`, `libgcc_s.so.1`, `libc.so.6`.
요구 버전: GLIBC ≤ 2.38, GLIBCXX ≤ 3.4.32(GCC 13). 크기 2.6 MB(strip 2.4 MB). 장치에는 whisper-server(또는 whisper-cli, aarch64 빌드는
`native/.deps/arm64-tools/bin`), 모델 파일, `yamnet-embeddings.onnx`, 헤드 JSON, fusion 정책, `contracts/`, device-capabilities가 필요하다.

## 결정이 필요한 것

- 장치 배치 경로(설정 파일, 모델, whisper 바이너리, `base_dir`)와 systemd 유닛(RuntimeDirectory, FIFO 경로) — IPK 단계
- Pi에서 whisper-server 상주(메모리 사용, 지연 감소) vs whisper-cli(발화마다 모델 로드)
- (해결) 다른 노드에서 누름 통화: `pfn/v1/ai/voice/listen` (계약 160e5c4, core 0.6.1 ACL c2dcdf9)
- 원격 `cancel`을 "녹음 버림"으로 했다(로컬 `stop`은 "바로 끝내고 전사"). UI가 "다시 누르면 끝내고 보내기"를 원하면 바꿔야 함
- 원격 누름의 `trace_id`를 발화 trace로 씀(힌트·task/llm 요청이 UI 누름과 같은 trace). 다른 의미를 원하면 알려 줄 것
- 카메라 재실을 fusion에 넣는 카메라: `presence_cameras` 기본 CAM-INDOOR-01만(현관 카메라 OUTDOOR가 방 위치를 덮지 않게)
- 부분 인텐트: Vosk 메모리(PC RSS +약 510 MB)·지연을 Pi5에서 재고 켤지 결정(P9). 안 되면 아키텍처 3-1 대안(whisper.cpp 짧은 창 partial)
- MQTT 보관 시간(core task/request 10 s)이 괜찮은지, 이 보관을 `hm::Bus`(hm_common)로 옮길지
- aarch64 빌드·QEMU 시험(이 작업에서 못 함)
