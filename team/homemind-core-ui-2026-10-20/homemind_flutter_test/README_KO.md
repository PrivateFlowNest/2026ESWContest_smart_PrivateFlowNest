# HomeMind Flutter 테스트 UI

이 폴더는 노트북에서 화면과 시나리오를 먼저 시험한 뒤 같은 Dart 소스를
Raspberry Pi 5의 Atlas Flutter 앱으로 옮기기 위한 작은 시제품이다.

## 지금 들어 있는 기능

- 목표 화면 크기: 1024×600
- UI 설계서 기준 하단 메뉴: `홈 · Task · 기기 · 개인정보 · 더보기`
- Context 신뢰도·판단 근거·Task 제안 중심의 홈 대시보드
- Task 분류 필터와 `감지 → 제안 → 준비 → 확인 → 실행 → 완료` 상세 흐름
- 기기 위치·분류·공식 ID·제어 ESP32·마지막 신호 상세 화면
- 개인정보 모드, 카메라·마이크·센서 수집 상태와 로컬 보관 안내
- 개인정보 스위치 변경을 Pi4에 실제 전송하고 확정된 상태를 다시 수신
- QR 기기 연결, 카메라, 피드백, 기록, 시스템 상태 화면
- 일반·Task 결과 피드백을 Pi4 SQLite에 실제 저장
- P0 발생 시 다른 화면을 덮고 ACK 전까지 유지되는 긴급 화면
- 오프라인에서도 마지막 상태를 유지하고 가능·제한 기능을 설명하는 안전 모드
- 일반 오류와 오프라인은 주황색, 빨간색은 P0에만 사용
- 귀가·취침·야간 이탈 Task를 Pi4 MQTT로 실제 전송
- Pi4 snapshot과 실시간 Task 상태 수신
- 실제 revision·action token을 사용한 승인·거절
- Task 상세에 실제 intent·대상 장치·신뢰도·revision·ACK 결과 표시
- Pi4 연결 끊김 표시와 3초 간격 자동 재연결
- P0 안전 경고와 확인 화면
- 기기별 온라인·작동 상태와 FND 표시값
- 명령 실패 코드·원인·재시도 안내 화면
- 센서값, 공식 출처 장치 ID, 마지막 수신 시각 표시
- heartbeat가 15초 끊긴 장치의 오프라인 표시
- Pi4 시간이 확인되지 않을 때 잘못된 날짜 대신 `시간 미확인` 경고 표시
- SQLite 최근 작업 최대 100개와 `ACK_TIMEOUT` 실패 이유 표시
- Pi4 전체 상태, MQTT·SQLite 구성요소, DB 크기, 저장공간 70% 경고 화면
- `device-registry.json`과 Flutter 공식 장치 ID 21개 자동 일치 검사

### 2026-10-22 추가 (homemind-core 0.6.0 · registry v3)

- `device_control` Task를 사람이 읽는 제목으로 표시: `거실 조명 켜기`, `현관문 열기`,
  `TV 음소거(깜빡임)`. 시나리오는 `귀가 준비`·`취침 준비`·`야간 이동 조명`·
  `조용 모드 켜기`·`조용 모드 해제` (`lib/src/task_presentation.dart`)
- 출처 배지: `음성 명령`(voice_parser) · `AI 제안`(llm) · `휴대폰`(ui, NODE-RPI4-01) ·
  `상황 감지`(context_rule) · `화면`(ui, NODE-RPI5-01)
- 승인 대기 Task(특히 LLM 제안)는 홈 오른쪽에 `에어컨을 켤까요?` 카드와 `실행`·`취소`
  버튼으로 표시. 기존 approve/reject `ui/action`(revision + action_token)을 그대로 보낸다.
  새 AI 제안이 오면 홈 탭으로 돌아온다
- `pfn/v1/ai/llm/result` 구독: `clarify` 질문과 실패 상태를 홈에 메시지로 표시 (전사 없음)
- `pfn/v1/ai/perception` ACTIVITY_CONTEXT 구독 + snapshot `world_state`의
  `{zone}.ACTIVITY_CONTEXT`: `회의 중 · 화상회의(스피커)` 표시
- 조용 모드 배너: 최신 `quiet_mode_on` 완료(이후 `quiet_mode_off` 없음) 또는 perception
  `quiet_required=true`. LED telemetry `light_pattern`(off/steady/blink)을 기기 값으로 표시
- 거절 사유 한국어 표시: `DEVICE_NOT_WIRED` → `미배선 기기` 등 7개 코드 + `ACK_TIMEOUT`
- 기기 목록 registry v3: MIC·CAM 담당 `NODE-RPI5-02`, 서보 MG90S, FAN-01 켜기/끄기만,
  `LED-LIVING-01` 거실 조명 D7, `LED-GUIDE-01`·`SERVO-BED-CURTAIN-01`은 `미배선`,
  FND 3개는 `미정`. `tool/check_device_catalog.py`가 ID·담당 제어기·wiring_status·
  서보 각도를 registry와 비교한다
- 키오스크: `HOMEMIND_KIOSK=true` 또는 URL `?kiosk=1`이면 정확히 1024×600으로 그리고
  창 크기에 맞춰 확대·축소, 스크롤바 숨김, 마우스 드래그 스크롤 허용

### 2026-10-22 추가 (말하기 버튼 · `pfn/v1/ai/voice/listen`)

- 상단바(모든 탭, 홈 포함)의 `말하기` 버튼 → `pfn/v1/ai/voice/listen`에 `{action:"start"}`
  발행 (QoS 1, retain 안 함, `source_node_id` NODE-RPI5-01 → `target_node_id` NODE-RPI5-02,
  `client_id`, 새 `event_id`·`trace_id`, `expires_at_ms` = 보낸 시각 + 3초). 음성·텍스트는
  싣지 않는다. 계약: `contracts/pfn.v1/voice-listen.schema.json` (`lib/src/voice_listen.dart`)
- 누르면 `듣는 중…`으로 바뀌고 6초 뒤, 또는 그 전에 새 음성·AI Task나 새
  `ai/llm/result`(되묻기 포함)가 오면 `말하기`로 돌아온다. 듣는 중에 다시 누르면 `cancel`
  (같은 trace_id). Pi4 연결이 끊겼거나 개인정보 모드에서 마이크가 꺼져 있으면 비활성
- 가상 모드: 1.5초 뒤 `거실 조명 켜기` 음성 Task가 들어온다 (`simulateVoiceReply`)
- `client_id`: `--dart-define=HOMEMIND_CLIENT_ID=homemind-ui-7inch` (기본
  `homemind-ui-laptop-01`, `[A-Za-z0-9_-]` 64자 이하만 허용)
- 네이티브 실행(Pi5 eLinux, Windows)은 환경변수 `HOMEMIND_PI4_HOST`,
  `HOMEMIND_MQTT_TCP_PORT`, `HOMEMIND_KIOSK`, `HOMEMIND_CLIENT_ID`,
  `HOMEMIND_DATA_MODE`로 빌드 값을 덮어쓴다. 웹은 기존처럼 URL이 우선
- 실제 브로커 확인 (PC 데모 브로커, 시뮬레이션):
  `flutter test test/voice_listen_live_test.dart --dart-define=RUN_BROKER_LIVE=true`
- 주의: RPI5-02는 `expires_at_ms`가 지난 요청을 버린다. 두 Pi5의 시계가 3초 이상
  어긋나면 버튼이 무시된다 (NTP 등 시간 동기 필요)

### 2026-10-22 추가 (네이티브 MQTT 로그인 · Pi4 1884)

- 사용자 이름: 환경변수 또는 `--dart-define` `HOMEMIND_MQTT_USERNAME` (예: `NODE-RPI5-01`)
- 비밀번호: **파일로만** — `HOMEMIND_MQTT_PASSWORD_FILE`(환경변수 또는 dart-define, 경로만)의
  첫 줄, 앞뒤 공백·탭 제거 (C++ 노드 `hm::BrokerConfig::password()`와 같은 규칙). 연결할
  때마다 다시 읽는다. 값을 받는 `HOMEMIND_MQTT_PASSWORD`(환경변수·dart-define)가 있으면
  연결하지 않고 화면에 설정 오류를 표시한다. 오류 문구에는 비밀번호가 나오지 않는다
- 둘 중 하나만 있거나 파일을 못 읽으면 연결하지 않고 이유를 표시한다(파일 문제는 3초마다
  다시 시도). 브로커가 거부하면 `Pi4 MQTT 인증 거부`
- 웹 빌드는 그대로 익명(케이블 전용 1883/9001 개발 리스너)
- 실제 브로커 확인 (PC 시뮬레이션, Docker): `bash tool/mqtt_auth_live.sh` — 임시 Mosquitto
  (`allow_anonymous false`, `../package/mosquitto.acl`) + homemind-core(NODE-RPI4-01) 위에서
  익명·틀린 비밀번호 거부, UI 계정의 state 요청·응답과 voice/listen 발행, ACL(UI 계정의
  `system/safety/alert` 발행 차단)을 확인한다. 비밀번호는 실행마다 무작위로 파일에만 쓰고
  출력하지 않으며 끝나면 지운다. `ELINUX_LABEL=<label>`이면 eLinux 번들을 기기 키트 방식으로
  설치해 같은 브로커에 붙인 화면도 찍는다
- 주의: ACL상 NODE-RPI5-01은 `pfn/v1/system/safety/alert`를 쓸 수 없어서 `더보기`의 P0 수동
  시험 버튼은 1884 인증 브로커에서는 전달되지 않는다 (케이블 개발 브로커에서만 동작)

### 데모 실행 (Windows Flutter)

```powershell
.\CHECK_APP_WIN.cmd                     # 카탈로그 검사 + analyze + test
.\RUN_DEMO.cmd                          # 브로커 ws://127.0.0.1:9001 (PC 리허설)
.\RUN_DEMO.cmd ws://10.10.10.2:9001     # 팀 Pi4 브로커
.\RUN_DEMO.cmd mock                     # 브로커 없이 가상 데이터 + 새 Task 예시
```

`RUN_DEMO.cmd`는 web release를 `--no-web-resources-cdn`으로 빌드해
`http://127.0.0.1:8765`에서 제공하고 Chrome/Edge를 1024×600 앱 창으로 연다.
`set HOMEMIND_KIOSK_BROWSER=1`이면 전체 화면(`--kiosk`), `HOMEMIND_SKIP_BUILD=1`이면
기존 빌드 재사용, `HOMEMIND_NO_BROWSER=1`이면 서버만 띄운다. 같은 빌드에서도 URL로
바꿀 수 있다: `?broker=ws://10.10.10.2:9001&kiosk=1`, `?mode=mock&seed=1`.
가상 모드의 `더보기 → 시나리오 시험`에 음성 명령, AI 제안, AI 되묻기, 현관문 승인,
미배선 거절, 휴대폰 명령, 회의 감지(조용 모드 켜기/해제) 버튼이 있다.
`CHECK_APP.cmd`는 WSL `Ubuntu`의 Flutter가 없으면 `CHECK_APP_WIN.cmd`로 넘어간다.

주의: Flutter web은 한글 대체 글꼴을 실행 중 `fonts.gstatic.com`에서 받는다.
인터넷이 없는 데모 망에서는 한글이 네모로 보일 수 있으니 리허설 PC가 인터넷에 연결되어
있는지 확인한다.

기본 실행은 `MqttHomeMindDataSource`를 사용한다. 랩탑 웹 미리보기는 Pi4의
`ws://10.10.10.2:9001`, 네이티브 앱은 MQTT TCP `10.10.10.2:1883`을 사용한다.
`MockHomeMindDataSource`도 남아 있어 Pi4 없이 화면만 시험할 수 있다.

## 노트북에서 실행

Windows 정책이 내려받은 `dart.exe`를 차단하기 때문에 Flutter 3.47.2를 이 PC의
Ubuntu(WSL)에 설치해 검증했다. 다음 한 줄로 분석과 자동 테스트를 다시 실행한다.

```powershell
.\CHECK_APP.cmd
```

화면을 다시 빌드하고 브라우저에서 보려면 다음 파일을 실행한다.

```powershell
.\RUN_PREVIEW.cmd
```

Pi4가 없는 날에는 아래 파일을 실행한다. 가상 센서값과 실패 버튼이 표시된다.

```powershell
.\RUN_LAPTOP_PREVIEW.cmd
```

그 뒤 브라우저에서 `http://localhost:8765`를 연다. 실행 창 크기를 1024×600
근처로 맞추고 각 버튼을 한 번씩 누른다. `취침 제안`은 최근 작업 카드에서
`승인` 또는 `거절`을 눌러야 다음 상태로 바뀐다. 미리보기 서버를 끝낼 때는
명령 창에서 `Ctrl+C`를 누른다.

## 실제 Pi4와 연결할 때

Pi4와 랜선을 연결하고 Pi4가 `10.10.10.2`인지 확인한 뒤 `RUN_PREVIEW.cmd`를
실행한다. 화면 상단에 `Pi4 연결됨`이 표시되면 실제 모드다.

1. 연결 직후 `pfn/v1/core/state/request`로 snapshot을 요청한다.
2. `pfn/v1/core/state/response/{client_id}`를 화면 상태로 변환한다.
3. 시나리오 버튼은 `pfn/v1/core/task/request`를 발행한다.
4. 승인·거절은 `pfn/v1/ui/action`을 발행한다.
5. `pfn/v1/core/task/update`로 상태를 실시간 갱신한다.
6. 연결이 끊기면 오프라인으로 표시하고 3초 뒤 자동 재연결한다.

브라우저용 `9001`은 PC–Pi4 직접 랜선 개발에서만 쓰는 익명 포트다. MQTT
비밀번호를 Flutter 코드나 웹 빌드에 저장하지 않는다. 최종 Pi5 Wi-Fi 배포에서는
인증된 `1884` 접속 정보를 C++ 브리지나 안전한 시스템 저장소에서 관리한다.

통신만 빠르게 재검사하려면 다음 명령을 실행한다.

```powershell
wsl.exe -d Ubuntu -- bash -lc "cd '/mnt/c/Users/junwo/Documents/Codex/2026-08-26/pi-4-pi4-sql-lite/homemind_flutter_test' && ~/.local/opt/flutter/bin/dart run tool/mqtt_websocket_smoke.dart"
```

성공하면 `MQTT_WEBSOCKET_UI_SMOKE_PASS`가 표시된다.

## Pi5 Atlas로 옮기는 방법

Windows에서 만들어진 `.exe`를 복사하는 것이 아니라 이 폴더 전체를 Pi5용 Atlas
빌드 환경으로 옮겨 ARM64 바이너리를 다시 빌드한다. `atlas/`에는 공식 샘플을
기준으로 앱 ID와 실행 파일명을 HomeMind용으로 바꾼 골격도 포함되어 있다.

현재 보유한 공식 샘플 위치:

`C:\Users\junwo\OneDrive\Desktop\ESW\ai-native-os-sample-flutter`

Atlas 앱 ID는 `com.atlas.app.homemind_ui`, 실행 파일명은
`homemind_flutter_test`로 설정했다. 현재 UI에는 플랫폼 플러그인이 없으므로
Bluetooth·audio·video 권한도 요구하지 않는다. 실제 D-Bus 연결을 붙일 때만
필요한 권한과 패키지를 추가한다.

## Pi5용 eLinux 번들 (Atlas SDK 없이, 오픈소스 flutter-elinux) — PC 시뮬레이션만

Atlas SDK(flutter-elinux-atlas, arc)가 없어서 `atlas/`의 원본인 Sony 오픈소스
flutter-elinux로 Wayland 번들을 만든다. `atlas/runner`는 flutter-elinux 템플릿 runner와
바이트 단위로 같고, 다른 점은 Atlas 설치 경로 RPATH와 앱 ID뿐이다. 빌드 도구는
`tool/elinux/`에 있고 결과물은 git에 넣지 않는 `<repo>/native/.deps/ui-elinux/out/<label>`에
쓴다. Docker Desktop(arm64 QEMU binfmt)이 필요하다.

```bash
bash tool/elinux/build_elinux.sh images               # 빌드·실행 이미지 (Ubuntu 24.04)
RUN_TESTS=1 bash tool/elinux/build_elinux.sh build sony-3.29.3
bash tool/elinux/build_elinux.sh run sony-3.29.3 x64     # weston headless + 스크린샷
WAIT_SECONDS=75 bash tool/elinux/build_elinux.sh run sony-3.29.3 arm64   # QEMU
```

- flutter-elinux: `github.com/sony/flutter-elinux` main `cdc96226` → Flutter 3.29.3
  (Dart 3.7.2), eLinux 엔진 `cf56914b32` (`sony/flutter-embedded-linux` 릴리스)
- 막힌 점과 처리: 이 앱의 `mqtt_client ^10.11.11`은 Dart 3.8 이상이 필요해 그대로는
  pub get이 실패한다. 빌드용 작업 복사본에서만 `dependency_overrides: mqtt_client: 10.8.0`
  (Dart 3.3 이상 지원 마지막 버전)을 넣는다. 저장소 `pubspec.yaml`·`pubspec.lock`은 바꾸지
  않는다. 이 조합에서도 analyze 0, 앱 테스트 전부 통과(Linux x64 flutter-tester)
- 빌드: x86_64 컨테이너에서 clang 18 + Ubuntu arm64 multiarch 라이브러리로 aarch64
  교차 빌드, x64 번들도 같이 만든다. `--dart-define=HOMEMIND_KIOSK=true
  HOMEMIND_CLIENT_ID=homemind-ui-7inch`, 브로커 기본값 `10.10.10.2:1883`
- 실행 확인: weston 13 headless(pixman, kiosk shell, 1024×600) + Mesa 소프트웨어 EGL에서
  `HOMEMIND_PI4_HOST=host.docker.internal`로 PC 데모 브로커에 붙여 스크린샷
  (`run-x64/`, `run-arm64/screenshot-1024x600.png`)
- 실행: `./homemind_flutter_test --bundle=. --fullscreen --width=1024 --height=600`
  (번들 RUNPATH `$ORIGIN/lib`, 압축을 푼 어느 위치에서나 실행)
- 기기에 있어야 하는 것: Wayland 컴포지터(xdg-shell, 예: weston/labwc/sway), EGL + GLES2
  드라이버(Pi5는 Mesa V3D), `libwayland-client0` `libwayland-cursor0` `libwayland-egl1`
  `libxkbcommon0` `libegl1` `libgles2` `libfontconfig1`, 시간대 설정(화면 시계), MQTT 브로커
  접근. 한글 글꼴은 번들에 들어 있다. runner는 Ubuntu 24.04에서 빌드해 glibc 2.38
  (`__isoc23_strtol`)·libstdc++ GLIBCXX_3.4.32가 필요하다(다른 RPI5 네이티브 패키지와 같은
  전제). 엔진 .so는 glibc 2.18이면 된다. 기기 glibc가 더 낮으면 runner만 낮은 sysroot에서
  다시 빌드한다
- Atlas OS 위에서 이 번들이 Atlas 컴포지터·권한 모델과 함께 도는지는 실기에서만 알 수 있다

### 기기 실행 키트 (`tool/elinux/device/`, IPK `homemind-rpi5-ui`용)

| 파일 | 기기 위치 | 내용 |
|---|---|---|
| `homemind-ui-elinux-wayland-arm64.tar.gz` | `/data/share/homemind/ui/` | `tar --strip-components=1`로 `bundle/` 안쪽만 |
| `homemind-ui-launch.sh` (0755) | `/data/share/homemind/ui/` | ui.env를 셸 평가 없이 읽어 허용된 `HOMEMIND_*`만 넘기고 전체 화면 실행 |
| `ui.env` (conffile, 0644) | `/data/share/homemind/config/ui.env` | 브로커 `BROKER_HOST`(자리표시)·1884, `NODE-RPI5-01`, 비밀번호 파일 경로, `homemind-ui-7inch`, 키오스크 |
| `homemind-ui.service` (0644) | systemd 유닛 디렉터리 | `User/Group=network`, `After=network-online.target`, `Restart=always` |

- 비밀번호 파일 `/data/share/homemind/secrets/mqtt-password`(0640 root:network)는 IPK에 넣지
  않는다. `deploy/NODE-RPI5-0X/install.sh` 5단계와 같은 방법으로 기기에서 만든다
- launcher는 `BROKER_HOST` 자리표시가 남았거나, `*PASSWORD*` 키·모르는 키가 있거나, 비밀번호
  파일을 못 읽거나, Wayland 소켓이 없으면 이유를 출력하고 끝낸다(systemd가 5초 뒤 재시작).
  `docker run --rm -v "<tool/elinux/device>:/device:ro" homemind-ui-elinux-builder:24.04
  sh /device/test_launcher.sh`로 9가지 경우를 확인한다
- 유닛의 `[판단]` 줄(컴포지터 유닛 이름, `XDG_RUNTIME_DIR`·`WAYLAND_DISPLAY`, GPU 그룹
  `video render`, 실행 계정)은 Atlas 기기에서 확인한 뒤 고친다

## Pi5에서 성공 판정

- 1024×600 화면이 잘리지 않는다.
- 터치로 모든 버튼이 눌린다.
- 10분 이상 실행해도 화면이 멈추지 않는다.
- Wi-Fi를 끊으면 오프라인, 다시 켜면 연결됨으로 바뀐다.
- Pi4 재부팅 후 snapshot을 다시 받아 기존 작업과 장치 상태가 복원된다.
- 최종 단계에서 V4L2 비전 프로세스와 UI를 함께 실행해도 프레임 끊김이 심하지 않다.

현재 Flutter 3.47.2에서 코드 분석 오류 0개, 자동 테스트 16개 통과와 실장비 전용
테스트 1개 보류 상태다. (2026-10-22 Windows Flutter 3.47.2 PC 검사: 42개 통과,
실장비 전용 1개 보류. 말하기 버튼 추가 후 54개 통과, 실장비·브로커 전용 2개 보류. MQTT 로그인 추가 후 62개 통과, 실장비·브로커 전용 5개 보류. core 0.6.0 새 Task 화면은 가상 데이터와 계약 예제로만 확인했고
실제 Pi4·Pi5 실기 확인은 아직이다.) 웹 release 빌드, 세 시나리오의 Pi4 Task·ACK 왕복,
개인정보 변경, 피드백 저장, P0 확인 및 해제, 15초 오프라인 판정과 MQTT 재연결을
실제 Pi4에서 확인했다. 상세 결과는 `../reports/ui-pi4-integration-20260904.md`에 있다.
Pi5 ARM64 빌드, Atlas 권한, Wayland 전체 화면과 실제 터치는 Pi5가 준비된 날 최종
확인한다.
