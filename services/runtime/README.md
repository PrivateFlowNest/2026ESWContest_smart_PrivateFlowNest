# homemind-runtime — NODE-RPI5-02 perception (PC 리허설)

PC 기준 AI 서비스를 pfn.v1 MQTT 계약에 연결하는 데모 런타임이다. 이 문서는 `perception` 명령(RPI5-02 역할)만 다룬다.

```
마이크 1개 (sounddevice 16 kHz mono, 100 ms 블록) ── 같은 프로세스 안에서 두 소비자로 분배
 ├─ 상황 경로 (항상 동작): 4초 창 / 1초 간격 → YAMNet(로컬 SavedModel) + VAD 특징 → v6 activity 헤드
 │     → AUDIO_CONTEXT → context-fusion(v6 정책) → pfn/v1/ai/perception ACTIVITY_CONTEXT
 │       전이(ACTIVATED/UPDATED/DEACTIVATED) 때마다 + 10초마다 현재 상태 재발행(UNCHANGED)
 └─ 음성 경로: 에너지 VAD 끝점 검출 → 임시 16 kHz PCM16 WAV → whisper.cpp → 결정론적 파서(VoiceParser)
       → pfn/v1/core/task/request 또는 pfn/v1/ai/llm/request (임시 WAV는 전사 직후 삭제)
```

core는 `context-rules.json`에 따라 `quiet_required=true + ACTIVATED` → `quiet_mode_on`, `quiet_required=false + DEACTIVATED` → `quiet_mode_off`를 만든다.

## 준비

- venv: `services/runtime/.venv` (exaone-edge, audio-context, context-fusion, speech-to-text, partial-intent가 `pip install -e`로 설치됨)
- 모델(모두 로컬, 실행 중 인터넷 접속 없음): YAMNet `services/edge-ai/audio-context/artifacts/yamnet-tfhub-1-savedmodel/`, 헤드 `.../public-balanced-v6-ensemble-50-50/activity.joblib`, 정책 `services/edge-ai/context-fusion/config/audio-policy-v6.3-hold15.json`(해제 15회 유지, 되돌리기용 v6.2.1 `audio-policy-v6-ensemble-50-50.json`), whisper `services/edge-ai/speech-to-text/models/whisper-bin-x64-b4938/Release/whisper-cli.exe` + `ggml-whisper-small-homemind-lora-v4-q5_0.bin`
- 설정: `config/runtime.pc.example.json`을 복사해서 쓴다. 브로커 `127.0.0.1:1883` 익명. 비밀번호는 파일에 쓰지 않고 `broker.password_file` 경로만 둔다. 상대 경로는 저장소 루트 기준

## 실행 (PowerShell, `services/runtime`에서)

```powershell
$py = ".\.venv\Scripts\python.exe"

# 1) 마이크 목록 (* = 기본 장치)
& $py -m homemind_runtime.perception --list-devices

# 2) 마이크 실행: 설정의 audio_device(번호 또는 이름 일부)를 쓰거나 --device로 지정
& $py -m homemind_runtime.perception --config config\runtime.pc.example.json --device "USB"
#    lead CLI도 같은 서비스를 부른다 (설정 파일 값만 사용)
.\.venv\Scripts\homemind-runtime.exe perception --config config\runtime.pc.example.json

# 3) 파일 모드 (마이크 없이). --fast = 최대 속도, 없으면 실시간 재생 속도
& $py -m homemind_runtime.perception --config config\runtime.pc.example.json --wav scene.wav --fast --stdout
#    긴 녹음은 구간만: --offset 1558.9 --seconds 40 / 여러 파일 이어 붙이기: --wav a.wav --wav b.wav
```

주요 옵션 (설정 키 / 명령행)

| 설정 키 | 명령행 | 기본값 | 설명 |
|---|---|---|---|
| `audio_device` | `--device` | 기본 입력 | 번호 또는 이름 일부. 16 kHz를 못 받는 장치는 기본 샘플레이트로 열고 16 kHz로 변환 |
| `listen_mode` | `--listen-mode` | `push_to_talk` | `push_to_talk` / `always` / `off`(음성 경로 끔). 상황 경로는 항상 동작 |
| `presence_zone` | `--presence-zone` | `UNKNOWN` | 카메라 없음 → 재실 UNKNOWN. 값을 주면 PRESENCE 입력으로 주입(아래 주의) |
| `publish` | `--stdout` | `mqtt` | `stdout`은 `토픽 JSON` 줄로 출력(브로커 없이 확인) |
| `wav_voice` | `--voice` | `auto` | 파일 모드 음성 경로: `whole`(파일 전체 = 발화 1개), `vad`(끝점 검출), `off`. `auto`는 30초 이하면 `whole` |
| `debug_transcripts` | `--debug-transcripts` | `false` | 리허설 전용. 켜야만 전사문을 화면·INFO 로그에 보인다 |
| `whisper_server_url` | — | `null` | 상주 whisper-server를 쓰면 발화당 약 0.5초 빠름(아래) |
| `vad.*` | — | 200 / 700 / 8000 / 400 ms | 시작 음성 길이 / 끝 무음 / 최대 / 최소, `threshold_db -45`, `noise_margin_db 10` |
| `republish_seconds`, `perception_ttl_seconds` | — | 10, 15 | 재발행 주기, `valid_until_ms` = 발행 시각 + 15초(재발행 사이 공백 없음) |

## 누름 통화(push-to-talk)와 스피커 반향

회의 장면은 스피커로 회의 소리를 틀고 마이크가 그것을 듣는다. 그래서 기본값은 `push_to_talk`이다.

1. 콘솔에 `Enter를 누르고 말하세요`가 보이면 Enter → `듣는 중...`
2. 말한다. 말이 끝나고 약 0.7초 조용하면 자동 종료, 다시 Enter를 누르면 즉시 종료, 최대 8초
3. 6초 안에 음성이 시작되지 않으면 취소(`음성이 감지되지 않았습니다`)
4. 결과 줄: `[voice] 2.0s, STT 2061 ms → task_request AIR_CONDITIONER TURN_ON` (전사문은 표시하지 않음)

Enter를 누르지 않은 동안 스피커 소리는 음성 경로에 들어가지 않는다. 다만 Enter 뒤 녹음에는 스피커 소리도 함께 들어가므로, 스피커 소리가 크면 무음 자동 종료가 늦어진다. 이때는 말한 뒤 Enter로 끝낸다. `always`는 스피커 소리를 명령으로 오인할 수 있으니 회의 장면에서는 쓰지 않는다.

## presence_zone 주의

- 기본 `UNKNOWN`: 회의 판단은 소리만으로 한다(`zone: "unknown"`, activity `MEETING`)
- `DESK`를 주면 zone `desk`가 되고 회의 확신도가 조금 오른다. 하지만 **책상에서 조용하면 STUDYING + quiet_required=true로 ACTIVATED된다**(fusion 정책: 책상·침대의 QUIET는 조용 필요). 회의 전 무음 구간에서 quiet_mode_on이 먼저 켜질 수 있으니, 회의 장면을 "회의 시작 → 조용 모드"로 보여주려면 `UNKNOWN`을 권장한다
- 이 값은 실제 인식이 아니다. fusion에는 `source: pc.scenario`, `simulated: true` PRESENCE 입력으로 들어간다

## 무엇이 시뮬레이션인가

- 파일 모드 결과는 항상 `simulated: true`(perception, llm/request)로 발행한다. 실기 성공으로 기록하지 않는다
- 마이크 모드는 설정 `simulated`를 따른다. 예제 설정은 PC 리허설이므로 `true`
- 재실(presence)은 카메라가 없어 UNKNOWN 또는 설정값이다. 시선·자세(vision) 경로는 없다
- PC 결과(YAMNet GPU 창당 수 ms, whisper CPU 발화당 약 1.5~2.7초)는 Pi5 성능이 아니다. Pi5 수치는 실기에서 다시 잰다
- PARTIAL_INTENT_HINT는 아직 발행하지 않는다

## 개인정보

- 원음은 저장하지 않는다. 발화 WAV는 발화마다 만드는 임시 폴더(`%TEMP%\hm-stt-*`)에 쓰고, whisper가 끝나는 즉시 파일과 폴더를 함께 지운다
- perception에는 결과만 들어간다(원음·전사문 없음). 전사문은 파서가 못 푼 경우에만 `ai/llm/request`로 RPI5-01에 간다
- 전사문은 `debug_transcripts`가 켜졌을 때만 화면·로그에 나온다

## whisper-server (선택, 지연 감소)

```powershell
cd ..\edge-ai\speech-to-text\models\whisper-bin-x64-b4938\Release
.\whisper-server.exe -m ..\..\..\artifacts\training\whisper-small-homemind-lora-v4\ggml-whisper-small-homemind-lora-v4-q5_0.bin -l ko -t 8 -ng --host 127.0.0.1 --port 8178
```

설정에 `"whisper_server_url": "http://127.0.0.1:8178"`. 끄면(`null`) 발화마다 `whisper-cli`를 실행한다. Linux 빌드는 `whisper_cli_path`만 바꾸면 된다.

## 시험

```powershell
.\.venv\Scripts\python.exe -m pytest tests -q --basetemp=$env:TEMP\hmpt\rt
```

VAD 끝점 검출(합성 음), fusion → perception 스키마 검증, 재발행 주기, 파서 3문장(불 켜줘 / 현관문 열어줘 / 아 좀 덥네), 임시 WAV 삭제, 누름 통화, 파일 모드(두 경로), 실제 YAMNet+헤드 로드.
