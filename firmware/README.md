# HomeMind 제어기 펌웨어 (ESP32 · ESP8266)

| 보드 | 노드 ID | 장치 |
|---|---|---|
| ESP32 DevKit | `CTRL-ESP32-ACT-01` | SERVO-DOOR-01 (GPIO19), SERVO-WINDOW-01 (GPIO21) 열기/닫기 모터. registry v4: SERVO-DOOR-02 철거, 팬은 ESP8266으로 |
| NodeMCU v2 (ESP8266) | `CTRL-ESP8266-LED-01` | LED-LIVING-01 (D0), LED-AIRCON-01 (D1), FAN-01 (D2). registry v4: 그 밖의 LED·FND는 모형에 없음 |

기준: `contracts/pfn.v1` (registry v3), `docs/handoff/reference/electrical-harness-plan.md`, 팀 `docs/ESP32_HANDOFF_KO.md`(registry v3와 다른 곳은 registry를 따름).

인형 1축 제어기(스테퍼 1개 + 원점 센서, USB 직렬 단독 동작)는 맨 아래 ["인형 1축 제어기"](#인형-1축-제어기-doll_axis-단독-동작) 절을 본다. pfn.v1·MQTT와 무관하다.

> **상태: 빌드·PC 시뮬레이션만 통과. 실제 보드에서는 아직 한 번도 돌려 보지 않았다.** 아래 "실기 미검증" 참고.

## 폴더

```
platformio.ini            env: esp32dev, nodemcuv2, native(PC 단위시험 + CLI), teensy41_axis, esp32_axis
include/secrets.h.example 비밀값 틀 (secrets.h는 gitignore)
include/hm_secrets_guard.h secrets.h 없으면 #error
include/doll_axis_config.h 인형 1축: 핀·기구 값 (전부 TODO 자리값)
lib/hm_device/            하드웨어 무관 로직: 명령 검증, 중복 차단, 만료, LED 상태, ACK/heartbeat/telemetry JSON, 보드 설정표
lib/hm_runtime/           Arduino 전용: Wi-Fi, MQTT, SNTP, 타이머 (두 보드 공용)
lib/hm_axis/              인형 1축: 사다리꼴 가감속 스텝 생성, 원점 복귀·리밋·센서 감시 상태기계, 줄 프로토콜 (하드웨어 무관 C++11)
src/esp32_actuator/       ESP32 HAL(LEDC 서보 50 Hz, 팬 GPIO)
src/esp8266_led/          ESP8266 HAL(LED GPIO)
src/native_cli/           PC CLI (check_messages.py가 사용)
src/doll_axis/            인형 1축 Arduino 진입점 (Teensy 4.1 / ESP32, STEP/DIR/EN/센서, Serial 115200)
test/test_hm_device/      Unity 단위시험 38개
test/test_hm_axis/        Unity 단위시험 33개 (가상 시간·가상 캐리지)
tools/check_messages.py   생성 메시지를 pfn.v1 스키마·sensor-rules·registry로 검사
tools/doll_axis_cli.py    인형 1축 호스트 CLI (pyserial) + test_doll_axis_cli.py (가짜 직렬 13개)
```

## 준비 (Windows, 한 번)

```powershell
py -3.12 -m venv firmware/.venv-pio
firmware/.venv-pio/Scripts/python -m pip install platformio==6.1.16
```

첫 빌드 때 PlatformIO가 플랫폼·툴체인·라이브러리를 받는다(espressif32 6.9.0 = Arduino-ESP32 2.0.17, espressif8266 4.2.1 = Arduino 3.1.2, ArduinoJson 7.2.1, 256dpi/MQTT 2.5.2). native env는 PC에 gcc가 없으므로 PlatformIO의 `toolchain-gccmingw32` 5.1을 받아 쓴다(`tools/native_toolchain.py`).

PlatformIO 출력에 한글 경로·기호가 있으면 깨질 수 있으니 PowerShell에서 `$env:PYTHONUTF8 = "1"`을 먼저 설정한다.

## secrets.h

```powershell
Copy-Item firmware/include/secrets.h.example firmware/include/secrets.h
```

`secrets.h`에 Wi-Fi(2.4 GHz), 브로커 주소·포트(인증 리스너 1884), 보드별 MQTT 비밀번호, 선택 SNTP 서버를 넣는다. MQTT 사용자명은 노드 ID(`mosquitto.acl`)라 펌웨어가 정한다. 값은 Pi4 담당자에게 보안 경로로 받고 **커밋·문서·채팅·로그에 쓰지 않는다**. 펌웨어는 값이 비었는지만 직렬 로그에 찍고 값 자체는 찍지 않는다. 파일이 없으면 빌드가 `#error`로 멈춘다.

## 빌드·시험

```powershell
cd firmware
.venv-pio/Scripts/pio run -e esp32dev
.venv-pio/Scripts/pio run -e nodemcuv2
.venv-pio/Scripts/pio test -e native
cd ..
py -3.12 firmware/tools/check_messages.py     # FIRMWARE_MESSAGES_PASS
```

`check_messages.py`는 native CLI를 빌드해 scenario-rules의 명령, 계약 예제 `door-command.json`, 잘못된 명령들을 넣고, 나오는 ACK·heartbeat·Last Will·telemetry 전부를 `contracts/validate-contracts.py`의 `validate()`와 sensor-rules로 검사한다. 보드 설정표(핀, capability, 서보 positions, 색, 배선 상태)와 프로토콜 상수도 registry·protocol.json과 비교한다. 결과 샘플: `firmware/tools/out/samples.jsonl`(gitignore).

## 업로드 (전장 규칙)

1. **ESP 보드의 VIN(5V) 점퍼(듀폰)를 뽑는다. USB로만 전원을 준다** (이중 공급 금지).
2. USB 연결 후 포트 확인: `.venv-pio/Scripts/pio device list`
3. 업로드:
   ```powershell
   .venv-pio/Scripts/pio run -e esp32dev -t upload --upload-port COM5
   .venv-pio/Scripts/pio run -e nodemcuv2 -t upload --upload-port COM6
   ```
   ESP32가 접속하지 않으면 BOOT 버튼을 누른 채 업로드를 시작한다.
4. 직렬 로그: `.venv-pio/Scripts/pio device monitor -b 115200`
5. USB를 뽑고 VIN 점퍼를 다시 꽂은 뒤 12V 전원으로 켠다. ESP 쪽 전원을 먼저 켜면 서보 튐이 줄어든다.
6. 예비 서보를 GPIO27에 꽂아 두지 않는다.

## 동작

### 부팅
registry v4 (사용자 시험 스케치와 같게): `setup()`의 첫 줄에서 ESP32는 현관문·창문 신호선을 LOW 출력으로만 두고(PWM 없음, 위치는 닫힘으로 가정) 첫 명령에서 LEDC를 붙인다(듀티를 먼저 넣고 핀 연결). ESP8266은 팬(D2), 거실 조명(D0), 에어컨 표시등(D1)을 LOW. 그 뒤에 Serial·Wi-Fi·MQTT를 시작한다. ESP32 ROM 부트 동안(펌웨어 실행 전) 신호선이 뜨는 구간은 펌웨어로 막을 수 없다.

### MQTT
- 클라이언트 ID `노드ID-runtime`, 사용자명 = 노드 ID, clean session, keepalive 10초, 재접속 3초 간격, Wi-Fi 20초 안 붙으면 재연결
- 구독 `pfn/v1/device/{node}/command` QoS 1. ACK QoS 1, telemetry·heartbeat QoS 0, retained 항상 false
- 접속 직후 heartbeat 1회 + 모든 장치 telemetry, 이후 heartbeat 5초, telemetry 10초 + 상태가 바뀔 때마다
- Last Will: `status: "offline"` heartbeat (접속 시점에 만들어 둠)
- 연결이 끊겨도 출력은 마지막 상태를 유지한다(서보를 움직이지 않음). 끊긴 동안 받은 명령은 없다(clean session)

### 명령 처리 순서
1. JSON·`command_id`·`trace_id`(UUID)·`target_device_id`가 없으면 ACK를 만들 수 없으므로 버리고 로그만
2. `command_id`가 최근 16개 안에 있으면 실행하지 않고 저장된 결과를 `duplicate: true`로 재전송 (거절 결과도 같음)
3. `schema_version`, `target_node_id`, 시간·`attempt` 오류 → `rejected_invalid`
4. 만료 → `rejected_expired` (`COMMAND_EXPIRED`)
5. 장치·동작·파라미터 오류 → `rejected_invalid`
6. 실행 → `succeeded` (출력 실패면 `failed`/`ACTUATOR_FAILED`). 결과를 기록한 뒤 ACK 발행

| error.code | 뜻 |
|---|---|
| `DEVICE_UNKNOWN` | 이 제어기 장치가 아님 |
| `DEVICE_NOT_WIRED` | registry에 있으나 미배선 (SERVO-BED-CURTAIN-01, LED-GUIDE-01, FND 3개) |
| `CAPABILITY_NOT_ALLOWED` | 지원 안 하는 동작 (예: FAN-01 `set_level`) |
| `PARAMETERS_INVALID` | 타입·범위 오류, 모르는 색·패턴, 잘못된 `auto_off_after_seconds` |
| `POSITION_NOT_REGISTERED` | registry 열림·닫힘 각도(0, 90)가 아님 |
| `COLOR_NOT_SUPPORTED` | `supported_colors`(off, red) 밖 (green, amber) |
| `NODE_MISMATCH`, `SCHEMA_VERSION_INVALID`, `COMMAND_INVALID` | 봉투 오류 |

`DEVICE_*`, `CAPABILITY_NOT_ALLOWED`, `PARAMETERS_INVALID`, `POSITION_NOT_REGISTERED`, `COLOR_NOT_SUPPORTED`는 core(`rules.hpp`)의 거절 코드와 같다.

### 시간·만료 규칙
- `secrets.h`의 `HM_NTP_SERVER`가 있으면 SNTP로 UTC를 맞춘다(인터넷 없으면 Pi4 등 LAN NTP 서버를 지정). 2024-01-01 이전 시각은 "미동기"로 본다
- 미동기 동안 `sent_at_ms`·`executed_at_ms`는 **0** (가짜 날짜를 만들지 않음). `uptime_ms`는 부팅 후 경과시간
- 유효시간 `ttl = min(expires_at_ms − issued_at_ms, 10000)`, 마감 `deadline = issued_at_ms + ttl`. 다음 중 하나면 만료이고 절대 실행하지 않는다
  1. 시계 동기 상태에서 현재 UTC ≥ deadline
  2. 수신 후 처리까지 ttl 이상 걸림
  3. 이번 MQTT 세션에서 본 가장 새 `issued_at_ms` + 그 뒤 경과시간(core 시계의 하한) ≥ deadline. 새 명령 뒤에 도착한 오래된 명령을 막는다. 재접속 때 초기화(Pi4 재부팅으로 시계가 바뀌어도 막히지 않게)
- 시계 미동기면 비행 중 지연은 알 수 없다. 최종 만료 시험은 SNTP를 켠 상태에서 한다(팀 인수인계서 7절)

### LED 규칙 (contracts/README "펌웨어가 지킬 LED 규칙")
- `set_switch {on}` = 전원 상태. 오버레이가 없으면 on 점등, off 소등
- `set_color {red, pattern: blink}` = 조용 오버레이: **1초 켜짐 / 1초 꺼짐**(주기 2초), 시작 순간 켜짐. 전원 상태와 무관
- `set_color {red, pattern: steady}` 또는 pattern 없음 = 오버레이 끔, 전원 상태로 복귀
- `set_color {off}` = 오버레이 끔 + 소등 (+ 예약된 자동 꺼짐 취소)
- `set_switch {on: true, auto_off_after_seconds: N}` = N초 뒤 자동 소등(정수 1~86400, LED만). 다른 `set_switch`가 오면 취소
- telemetry: `light_on` = 전원 상태, `light_pattern` = `blink`(오버레이) / `steady`(켜짐) / `off`

### telemetry
`readings`는 이름→값 하나씩이라 같은 이름을 쓰는 장치(문 2개, LED 5개)를 한 메시지에 넣을 수 없다. 그래서 **장치마다 한 메시지**를 보낸다. 서보 `door_position_degrees`/`window_position_degrees`(명령한 각도), 팬 `fan_level_percent` 0/100, LED `light_on`+`light_pattern`. 위치 센서가 없으므로 값은 명령한 출력 상태이고 `sensor_ready: true`.

## 실기 미검증 (사람이 확인할 것)
- 두 보드 모두 실제 업로드·부팅·Wi-Fi·브로커 접속·ACL 통과
- 열기/닫기 모터(registry v4, 2026-10-25): 사용자가 시험한 스케치와 같은 펄스(50 Hz, 1000~2000 µs, `write(90 + offset)`: 현관문 열기 -90 / 닫기 0, 창문 열기 -90 / 닫기 +90), 부팅 때는 신호 LOW로만 두고 첫 명령에서 PWM 시작. 펌웨어로 실제 모터가 같은 방향으로 도는지
- D4184 입력 HIGH = 팬 ON, LED 모듈 R 핀 HIGH = 점등
- Wi-Fi·브로커 주소는 `include/hm_site_network.h`(커밋, 사용자 결정 2026-10-25), MQTT 비밀번호만 `include/secrets.h`
- heartbeat 끊김 후 15~17초 offline, 재부팅·브로커 재시작 후 자동 복구, Last Will 도착
- SNTP 동기(서버 미정), 3초 안 ACK, 세 시나리오·조용 모드 실물 동작
- ESP8266은 MQTT 재접속(최대 수 초) 동안 LED 깜빡임이 잠깐 멈출 수 있음

## 인형 1축 제어기 (`doll_axis`, 단독 동작)

> **상태: PC 시뮬레이션·빌드만 통과. 보드·드라이버·모터·센서로는 한 번도 돌려 보지 않았다.**
> HomeMind(core·MQTT·pfn.v1) 연동 여부·방식은 **[미정]**이다. 그래서 `contracts/`·registry·기존 ESP 펌웨어는 건드리지 않았고, 이 제어기는 USB 직렬 줄 프로토콜로 아무 호스트(PC, Jetson, Pi)에서나 단독으로 움직인다.

배경(2026-10-22 사용자 결정): 인형 이동을 Octopus XY 대신 **한 축(앞/뒤), NEMA17 1개 + 광학 원점 센서 1개**로 바꾼다. 제어 보드는 Teensy 4.1이 유력하고 ESP32일 수도 있다. 스테퍼 드라이버는 미정(STEP/DIR 방식: TMC2209 standalone, A4988, DRV8825 등)이라 펌웨어는 STEP/DIR/EN 핀만 쓴다. `octopus/`의 Klipper 설정은 그대로 두었다.

### 빌드·시험

```powershell
cd firmware
$env:PYTHONUTF8 = "1"
.venv-pio/Scripts/pio run -e teensy41_axis   # Teensy 4.1 (platform teensy@5.0.0)
.venv-pio/Scripts/pio run -e esp32_axis      # ESP32 DevKit (espressif32@6.9.0)
.venv-pio/Scripts/pio test -e native         # hm_device 38개 + hm_axis 33개
.venv-pio/Scripts/python -m unittest discover -s tools -p test_doll_axis_cli.py   # CLI 13개
```

`secrets.h`는 필요 없다(Wi-Fi·MQTT 없음). 두 env는 `src/doll_axis/`와 `lib/hm_axis/`만 빌드한다. 업로드(`-t upload`, Teensy는 Teensy Loader)는 보드·배선이 정해진 뒤 사람이 한다.

### 배선 자리표 (값 미정 — 보드·드라이버·배선이 정해지면 채운다)

| 신호 | Teensy 4.1 기본 핀 | ESP32 기본 핀 | 연결 대상 | 비고 |
|---|---|---|---|---|
| STEP | 2 (TODO) | GPIO25 (TODO) | 드라이버 STEP | 3.3 V 로직 |
| DIR | 3 (TODO) | GPIO26 (TODO) | 드라이버 DIR | |
| EN | 4 (TODO) | GPIO27 (TODO) | 드라이버 EN | 기본 LOW = 켜짐(`enable_active_low`) |
| ENDSTOP | 5 (TODO) | GPIO33 (TODO) | 광학 센서 출력 | 내부 풀업. **3.3 V 신호만** (Teensy 4.1 핀은 5 V 불가) |
| GND | GND | GND | 드라이버 로직 GND, 센서 GND, 모터 전원 GND | 공통 접지 |
| 로직 전원 | 3.3 V | 3.3 V | 드라이버 VDD/VIO, 센서 VCC | TBD |
| 모터 전원 | — | — | 드라이버 VMOT | 전압·전원·VMOT 캐패시터 TBD |
| 모터 | — | — | 드라이버 A1/A2/B1/B2 → NEMA17 | 상 순서·전류 설정 TBD |

- 핀은 `include/doll_axis_config.h`에 있다. ESP32 기본값은 부트 스트래핑 핀(0/2/5/12/15), 플래시 핀(6~11), UART0(1/3), 입력 전용 핀(34~39, 내부 풀업 없음)을 피했다.
- 사람이 확인·결정할 것: 드라이버 종류와 마이크로스텝(→ `steps_per_mm`), 모터 전류, 센서 출력 극성(가려졌을 때 HIGH/LOW), 전원 켤 때 펌웨어가 돌기 전 EN이 떠 있는 동안 드라이버가 켜지는지(모듈의 EN 기본 풀업/풀다운), VMOT가 켜진 채로 모터 선을 꽂거나 빼지 않기.
- 이 배선은 확정 전장(`docs/handoff/reference/electrical-harness-plan.md`)에 없는 새 항목이다. 전장 문서 반영은 배선이 정해진 뒤 사용자가 결정한다.

### 프로토콜 (USB 직렬 115200 8N1, 한 줄 = 명령 하나, 줄 끝 `\n`)

정의: `lib/hm_axis/src/hm_axis_protocol.h`. 명령어는 대소문자 무관, 이름은 소문자로 바뀐다. mm는 소수 둘째 자리로 출력한다. 모든 명령 줄에는 **응답이 정확히 한 줄**(`OK ...` 또는 `ERR <CODE> <설명>`) 온다. 비동기 줄은 `EVT `로 시작한다.

| 명령 | 응답 | 나중에 오는 이벤트 |
|---|---|---|
| `HOME` | `OK HOME` | `EVT HOMED` 또는 `EVT ERROR HOME_FAILED no_trigger\|stuck_active` |
| `MOVE <name\|mm>` | `OK MOVE <label> <mm>` | `EVT ARRIVED <label> <mm>` (label = 이름 또는 목표 mm) |
| `STOP` (항상 받음) | `OK STOP` | 움직이던 중이면 `EVT STOPPED <mm>` |
| `ESTOP` (항상 받음) | `OK ESTOP` | 움직이던 중이면 `EVT ERROR ESTOP` |
| `POS?` | `OK POS <mm> <state> <homed 0\|1>` | |
| `STATUS?` | `OK STATUS state=.. homed=.. pos=.. endstop=0\|1 enabled=0\|1 error=..` | |
| `LIST` | `OK LIST <개수> <name>=<mm> ...` | |
| `SET <name> <mm>` | `OK SET <name> <mm>` (RAM만, 재부팅하면 기본값으로) | |
| `ENABLE 0\|1` | `OK ENABLE 0\|1` (0은 원점도 잊음) | |
| `VERSION?` | `OK VERSION doll_axis hm_axis/1.0.0` | |
| `HELP` | `OK HELP <명령 목록>` | |

- 부팅 때 `EVT BOOT doll_axis hm_axis/1.0.0`(설정이 틀리면 이어서 `EVT ERROR CONFIG_INVALID <필드>`). Teensy USB는 호스트가 포트를 열기 전 출력이 버려지므로 못 볼 수 있다.
- HOME·MOVE 하나는 반드시 `HOMED`/`ARRIVED`/`STOPPED`/`ERROR` 중 **하나**로 끝난다.
- state: `UNHOMED`, `HOMING`, `IDLE`, `MOVING`, `STOPPING`, `ERROR`.
- mm 문법 `[+-]숫자[.숫자]`(지수·nan·inf 없음, 정수·소수 각 6자리까지). 이름 `[a-z0-9_]{1,16}`, 숫자만으로 된 이름은 안 됨(숫자 mm와 구분). 이름은 8개까지.

| ERR / EVT ERROR 코드 | 뜻 |
|---|---|
| `NOT_HOMED` | 원점 복귀 전 MOVE |
| `OUT_OF_RANGE` | 소프트 리밋 `0..travel_mm` 밖 (MOVE, SET) |
| `BUSY` | 움직이는 중 HOME/MOVE/ENABLE (설명에 현재 state) |
| `UNKNOWN_POSITION` | 없는 이름 |
| `BAD_ARG`, `BAD_NAME`, `TABLE_FULL`, `UNKNOWN_COMMAND` | 인자·이름·표 가득·모르는 명령 |
| `LINE_TOO_LONG` | 95자 넘는 줄(통째로 버림), `BAD_LINE` 제어문자·비ASCII가 든 줄(통째로 버림) |
| `CONFIG_INVALID` | `doll_axis_config.h` 값이 검사에 걸림. 아무것도 움직이지 않음 |
| `HOME_FAILED` | `no_trigger`: 첫 탐색이 `travel_mm + home_margin_mm`(또는 재탐색이 후퇴 거리×2) 안에 센서를 못 찾음. `stuck_active`: 후퇴한 뒤에도 센서가 켜져 있음 |
| `ENDSTOP_HIT` | 움직이는 중 예상 밖 센서 → 즉시 정지 (뒤에 위치 mm) |
| `ESTOP` | ESTOP으로 움직임 중단 |

### 동작 규칙

- **부팅**: 드라이버 끔(EN 비활성), `UNHOMED`. 원점 복귀 전 MOVE는 `ERR NOT_HOMED`.
- **원점 복귀**: 빠른 탐색(`home_fast_mm_s`) → 센서가 걸리면 즉시 정지 → `home_backoff_mm` 후퇴(센서가 꺼져야 함) → 느린 재탐색(`home_slow_mm_s`) → 센서가 걸린 그 스텝을 원점(0, `home_dir=+1`이면 `travel_mm`)으로 삼고 그 자리에 선다. 시작할 때 이미 센서가 켜져 있으면 후퇴부터 한다.
- **가감속**: 사다리꼴(일정 가속도). 매 스텝 `v_next = min(최대속도, √(v²+2a), √(2a·(남은 스텝−1)))`, 간격 = 두 속도 평균의 역수(올림). 루프가 늦게 돌면 느려질 뿐 최대속도·가속도를 넘지 않는다. 스텝은 `loop()` 폴링으로 낸다(보드 실기 지터는 미확인).
- **센서 감시**(MOVING/STOPPING 중): 원점 끝에서 `endstop_zone_mm`보다 먼 곳에서 센서가 켜지거나, 원점 반대 방향으로 가다가 꺼졌던 센서가 다시 켜지면 즉시 정지 → `ERROR ENDSTOP_HIT`(스텝 잃음, 장애물, 배선 문제). 정지 상태에서는 감시하지 않는다. `endstop_debounce_us` 동안 같은 값이어야 인정한다.
- **STOP**: 설정 가속도로 감속해 멈춘다. 위치는 유지(`IDLE`). 원점 복귀 중이었으면 `UNHOMED`.
- **ESTOP**: 감속 없이 즉시 멈추고 드라이버를 끈다. 원점을 잊고 `ERROR`. `HOME`으로 복구.
- **드라이버 끄기**: `ENABLE 0`은 원점도 잊는다(손으로 밀 수 있으므로). `idle_disable_ms` 동안 움직임이 없으면 자동으로 끈다(원점은 유지, `unhome_on_idle_disable=true`면 잊음). 다음 HOME/MOVE가 다시 켠다.
- **ERROR**에서 MOVE는 `NOT_HOMED`. `HOME`이 복구한다(`CONFIG_INVALID`는 설정을 고쳐 다시 빌드해야 함).

### 보정 (모형에서 사람이, 부품이 정해진 뒤)

값은 모두 `include/doll_axis_config.h`의 TODO다. 바꾼 뒤 다시 빌드·업로드한다.

1. **steps_per_mm 계산값**: `(모터 스텝/회전 × 마이크로스텝) / (1회전당 이동 mm)`. 예: GT2 벨트 + 20T 풀리, 200스텝, 1/16 → `3200/40 = 80`; T8 리드스크루(리드 8 mm) → `3200/8 = 400`.
2. **센서 극성**: `ENABLE 0` 후 손으로 캐리지를 센서에 넣었다 빼며 `STATUS?` → 가렸을 때 `endstop=1`이어야 한다. 반대면 `endstop_active_high`를 뒤집는다.
3. **방향**: `HOME`이 센서 반대쪽으로 가면 바로 `ESTOP`, `invert_dir`를 뒤집는다. 센서가 먼 쪽 끝에 있으면 `home_dir = +1`.
4. **steps_per_mm 실측**: `HOME` → `MOVE 100` → 실제 이동 거리를 자로 잰다 → `새 값 = 지금 값 × 100 / 측정 mm`. 오차가 없어질 때까지 반복.
5. **travel_mm**: 원점에서 반대쪽 기구 끝까지 갈 수 있는 거리에서 몇 mm 여유를 뺀다. 작은 값에서 `MOVE`로 조금씩 늘려 확인한다.
6. **속도·가속도**: 작은 값에서 시작해 올린다. 여러 번 왕복한 뒤 `MOVE 0` → `STATUS?`에서 `endstop=1`, `MOVE <home_backoff_mm + 1>` → `endstop=0`이면 스텝을 잃지 않은 것이다. 소리가 나거나 어긋나면 낮춘다.
7. **이름 위치**: `MOVE <mm>`로 침대·욕실·현관 등 위치를 맞추고 `SET bed <mm>`로 시험한다(RAM만). 값이 정해지면 `kNamedPositions`에 적고 다시 빌드한다.

### 호스트에서 명령 보내기

`tools/doll_axis_cli.py`(Python 3.8+, `pip install pyserial`; Windows는 `.venv-pio`에 이미 있음). 명령마다 응답을 기다리고, HOME·MOVE는 끝 이벤트까지 기다린다. 끝 코드 0 = 성공, 1 = 제어기 오류(ERR, EVT ERROR, STOPPED, 재부팅), 2 = 시간 초과·포트·사용법 오류. 움직이는 중 Ctrl+C를 누르면 `STOP`을 보낸다.

```powershell
.venv-pio/Scripts/python tools/doll_axis_cli.py --list-ports
.venv-pio/Scripts/python tools/doll_axis_cli.py --port COM7 "VERSION?" HOME "MOVE bed" "POS?"
python3 tools/doll_axis_cli.py --port /dev/ttyACM0 -i      # Jetson/Pi, 대화형
```

포트 이름: Teensy는 Linux `/dev/ttyACM*`, ESP32 DevKit(CP210x/CH340)은 `/dev/ttyUSB*`(Linux 사용자는 `dialout` 그룹 필요), Windows는 `COMn`. ESP32는 포트를 열 때 리셋될 수 있어서 CLI가 열고 `--settle`(0.5초) 동안 부팅 출력을 읽고 버린다. 프로토콜 줄이 아닌 출력(ESP32 ROM 로그 등)은 `#`로 찍고 무시한다.

다른 프로그램에서 직접 보낼 때: 115200으로 열고 `"MOVE bed\n"`을 쓴 뒤, `OK`/`ERR` 줄을 기다리고, `OK`면 `EVT ARRIVED`/`EVT STOPPED`/`EVT ERROR` 중 하나를 기다린다. `EVT BOOT`가 오면 제어기가 재시작한 것이므로 다시 `HOME`해야 한다.

### 실기 미검증 (사람이 확인할 것)

- 보드·드라이버·센서·핀·전원 결정, 실제 업로드·부팅·USB 직렬 연결
- 드라이버 STEP 펄스 폭(기본 5 µs), DIR→STEP 준비 시간, EN 극성, 부팅 순간 EN 상태
- 실제 루프의 스텝 타이밍 지터(특히 ESP32), 최대 속도·가속도에서 스텝 잃음
- 광학 센서 극성·노이즈(디바운스 200 µs가 맞는지), 원점 반복 정밀도
- steps_per_mm, travel_mm, 이름 위치 값(전부 자리값)
- HomeMind 연동(MQTT·registry 장치 ID·계약) — **[미정]**, 정해지면 `contracts/`부터 고친다
