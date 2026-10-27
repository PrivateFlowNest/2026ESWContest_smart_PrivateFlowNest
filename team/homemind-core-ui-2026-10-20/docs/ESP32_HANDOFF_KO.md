# HomeMind ESP32 담당자 인수인계서

작성일: 2026-09-11  
대상: ESP32·ESP8266·Octopus 센서/액추에이터 담당자  
목적: 이 문서 하나를 기준으로 실제 센서와 액추에이터를 연결하고 Pi4 연동 시험까지 완료한다.

---

## 1. 한 줄 요약

Pi4 백엔드와 Flutter UI는 이미 구현·검증되어 있다. ESP32 담당자는 **공식 장치 ID를
그대로 사용해 heartbeat와 telemetry를 보내고, Pi4의 command를 실제 기기에 적용한 뒤
같은 command ID로 ACK를 반환**하면 된다.

```text
센서/액추에이터
      ↕
ESP32·ESP8266·Octopus
      ↕ MQTT
Pi4(중앙 백엔드 + SQLite)
      ↕ MQTT
Flutter UI(현재 노트북, 최종 Pi5)
```

Flutter UI는 ESP32에 직접 명령하지 않는다. 모든 명령과 기록의 기준은 Pi4다.

---

## 2. 현재까지 완료된 작업

마지막 실장비 통합 확인일은 2026-09-04이다.

- Pi4 주소: `10.10.10.2` (PC–Pi4 직접 랜선 개발 주소)
- Pi4 노드 ID: `NODE-RPI4-01`
- MQTT broker 설치 및 실행
  - 직접 랜선 개발용 MQTT TCP: `10.10.10.2:1883`
  - 노트북 Flutter 웹용 WebSocket: `ws://10.10.10.2:9001`
  - ESP32·Pi5 Wi-Fi용: `Pi4의 Wi-Fi IP:1884` (계정과 비밀번호 필요)
- Pi4 HomeMind Core 자동 시작 서비스 구현
- MQTT 메시지 원문과 처리 결과를 Pi4 SQLite에 저장
- 자동 데이터 삭제 비활성화
- 저장공간 사용률 70% 경고 구현
- Pi4 시간 오류 보호 구현
- heartbeat가 15초 끊기면 장치를 `offline`으로 판정
- 명령 ACK 제한 3초, 최대 3회 시도, 명령 유효시간 10초 구현
- 같은 MQTT 메시지의 중복 저장·처리 방지
- 세 시나리오의 Pi4 명령 생성 구현
- 가짜 ESP32·ESP8266를 이용한 command/ACK 왕복 성공
- 명령 실패, ACK 시간 초과, 잘못된 센서값 시험 완료
- P0 안전 경고와 사용자 확인 처리 완료
- Flutter UI에서 Pi4 연결·Task·기기 상태·센서값·오프라인·오류 표시 완료
- 개인정보 설정과 사용자 피드백의 Pi4 저장 완료
- SQLite 무결성 검사 결과 `ok`

가짜 장치로 통과한 시나리오:

1. `prepare_home` — 귀가 및 자동 환경 준비
2. `prepare_sleep` — AI 취침 준비
3. `night_bed_exit` — 야간 침대 이탈 및 이동 조명

따라서 새 백엔드나 새 MQTT 규격을 만들 필요가 없다. 기존 가짜 장치를 실제 펌웨어로
교체하는 것이 이번 작업의 핵심이다.

---

## 3. ESP32 담당자가 해야 하는 최종 작업

### 필수 작업

- [ ] 실제 보드별 담당 장치와 GPIO 핀 확정
- [ ] 센서·서보·LED·팬·FND의 단품 작동 확인
- [ ] Wi-Fi 연결 및 MQTT 자동 재연결 구현
- [ ] 보드별 고유 MQTT client ID 사용
- [ ] 자기 command 토픽 구독
- [ ] 5초마다 heartbeat 발행
- [ ] 실제 센서값 telemetry 발행
- [ ] Pi4 command 검증 후 실제 장치 작동
- [ ] 성공·실패·중복·만료 ACK 반환
- [ ] 같은 command ID가 재전송돼도 실제 동작은 한 번만 수행
- [ ] 전원을 끄면 15초 후 UI에서 offline이 되는지 확인
- [ ] 재부팅 후 자동으로 Wi-Fi와 MQTT에 재연결되는지 확인
- [ ] 세 시나리오를 실제 장치로 끝까지 실행
- [ ] 최종 핀맵, 펌웨어, 시험 로그를 팀에 전달

### 작업 완료의 정의

다음 경로가 실제 장치에서 한 번 이상 성공해야 한다.

```text
Flutter UI
  → Pi4 Task 승인
  → Pi4 command 발행
  → ESP32가 실제 장치 작동
  → ESP32 ACK 발행
  → Pi4가 Task 완료 처리
  → Flutter UI에 “완료했어요” 표시
```

---

## 4. 보드와 공식 ID 배정

ID는 대소문자까지 정확히 사용한다. 임의로 줄이거나 새 이름을 만들면 Pi4가 거절한다.

### 4.1 ESP32 액추에이터 제어기

노드 ID: `CTRL-ESP32-ACT-01`  
권장 MQTT client ID: `CTRL-ESP32-ACT-01-runtime`

구독 토픽:

```text
pfn/v1/device/CTRL-ESP32-ACT-01/command
```

발행 토픽:

```text
pfn/v1/device/CTRL-ESP32-ACT-01/ack
pfn/v1/device/CTRL-ESP32-ACT-01/telemetry
pfn/v1/system/heartbeat
```

| 실제 장치 | 장치 ID | 위치 | 받아야 하는 동작 |
| --- | --- | --- | --- |
| 현관문 서보 | `SERVO-DOOR-01` | 현관 | `set_position`, 0~180도 |
| 화장실 문 서보 | `SERVO-DOOR-02` | 화장실 | `set_position`, 0~180도 |
| 창문 서보 | `SERVO-WINDOW-01` | 거실 | `set_position`, 0~180도 |
| 화장실 환기 팬 | `FAN-01` | 화장실 | `set_switch`, `set_level` 0~100% |
| 침대 커튼 서보 | `SERVO-BED-CURTAIN-01` | 침대 | `set_position`, 0~180도 |

주의: 기존 자료에는 문·창문 서보가 `BG90S`, BOM에는 `MG90S`로 적힌 곳이 있다.
실물 각인을 확인한 후 팀 문서에서 하나로 통일해야 한다. 장치 ID는 변경하지 않는다.

### 4.2 ESP8266 LED·FND 제어기

노드 ID: `CTRL-ESP8266-LED-01`  
권장 MQTT client ID: `CTRL-ESP8266-LED-01-runtime`

구독 토픽:

```text
pfn/v1/device/CTRL-ESP8266-LED-01/command
```

발행 토픽:

```text
pfn/v1/device/CTRL-ESP8266-LED-01/ack
pfn/v1/device/CTRL-ESP8266-LED-01/telemetry
pfn/v1/system/heartbeat
```

| 실제 장치 | 장치 ID | 받아야 하는 동작 |
| --- | --- | --- |
| 세탁기 표시 LED | `LED-WASHER-01` | `set_switch`, `set_color` |
| 에어컨 표시 LED | `LED-AIRCON-01` | `set_switch`, `set_color` |
| TV 표시 LED | `LED-TV-01` | `set_switch`, `set_color` |
| 노트북 표시 LED | `LED-LAPTOP-01` | `set_switch`, `set_color` |
| 거실 조명 | `LED-LIVING-01` | `set_switch`, `set_color` |
| 야간 이동 조명 | `LED-GUIDE-01` | `set_switch`, `set_color` |
| 세탁기 소음 FND | `FND-WASHER-01` | `set_number`, 0~9999 |
| 에어컨 소음 FND | `FND-AIRCON-01` | `set_number`, 0~9999 |
| TV 소음 FND | `FND-TV-01` | `set_number`, 0~9999 |

FND 세 개의 ID는 확정됐지만 실제 핀과 제어기 배정은 아직 미확정이다. 이 문서의
ESP8266 배정을 기본안으로 사용하고, 핀이 부족하면 배선 전에 팀과 제어기 변경을 확정한다.

LED 색상 값은 다음 네 개만 사용한다.

```text
off, green, red, amber
```

### 4.3 Octopus XY 제어기

노드 ID: `CTRL-OCTOPUS-01`  
권장 MQTT client ID: `CTRL-OCTOPUS-01-runtime`

| 실제 장치 | 장치 ID | 역할 |
| --- | --- | --- |
| X축 NEMA17 | `MOTOR-XY-X-01` | `move_axis`, `home_axis` |
| Y축 NEMA17 | `MOTOR-XY-Y-01` | `move_axis`, `home_axis` |
| X축 원점 광센서 | `SENSOR-HOME-X-01` | `x_home_active` |
| Y축 원점 광센서 | `SENSOR-HOME-Y-01` | `y_home_active` |

인형 이동장치를 이번 단계에서 제외한다면 Octopus 부분은 보류해도 된다. 다만 해당
노드 ID를 ESP32나 ESP8266에 재사용하면 안 된다.

### 4.4 ESP32 담당 범위가 아닌 장치

- `CAM-INDOOR-01`: Pi5 실내 카메라
- `CAM-OUTDOOR-01`: Pi5 현관 카메라
- `MIC-INDOOR-01`: Pi5 마이크/소음 인식

카메라 프레임과 음성 원본은 MQTT로 보내지 않는다.

---

## 5. 반드시 지켜야 하는 MQTT 규칙

| 항목 | 확정값 |
| --- | --- |
| 스키마 버전 | `pfn.v1` |
| command/ACK QoS | 1 |
| telemetry/heartbeat QoS | 0 |
| retained | 항상 `false` |
| heartbeat 주기 | 5초 |
| 오프라인 판정 | 마지막 메시지 이후 15초 |
| ACK 제한시간 | 3초 |
| 최대 명령 시도 | 3회(최초 1회 + 재시도 2회) |
| 명령 유효시간 | 10초 |
| 최대 메시지 크기 | 256 KiB |
| `_ms` 시간 형식 | UTC Unix epoch millisecond |

### ID 규칙

- `event_id`: 메시지를 발행할 때마다 새 UUID
- `trace_id`: 한 시나리오가 끝날 때까지 동일한 UUID
- `command_id`: 하나의 실제 명령을 식별하는 UUID
- 같은 명령이 재전송될 때 `command_id`와 `trace_id`는 바뀌지 않는다.
- 재시도할 때 Pi4가 `attempt`만 1 → 2 → 3으로 올린다.
- ESP32의 `seq`는 재부팅 후 0부터 다시 시작해도 된다.
- MQTT client ID는 동시에 연결된 프로그램끼리 절대 같으면 안 된다.

### 보안 규칙

- Wi-Fi SSID·비밀번호와 MQTT 계정·비밀번호를 Git, JSON 예제, SQLite, 단체 채팅에 넣지 않는다.
- 실제 비밀번호는 Pi4 담당자에게 별도 보안 경로로 전달받는다.
- PC–Pi4 직접 랜선의 `1883`은 격리된 개발 연결에만 사용한다.
- ESP32가 일반 Wi-Fi에 붙을 때는 인증이 설정된 `1884`를 사용한다.

---

## 6. 부팅 후 펌웨어 동작 순서

다음 순서를 그대로 구현한다.

```text
1. 모든 출력 핀을 안전 상태(팬 OFF, LED OFF, 서보 급동작 금지)로 초기화
2. 저장된 node ID와 핀맵 확인
3. Wi-Fi 연결
4. 고유 MQTT client ID로 Pi4 broker 연결
5. 자기 node ID의 command 토픽을 QoS 1로 구독
6. 즉시 online heartbeat 1회 발행
7. 5초마다 heartbeat 발행
8. 실제 센서를 읽어 telemetry 발행
9. command 수신 시 검증
10. 실제 장치 동작
11. 실제 결과를 확인한 후 ACK 발행
12. Wi-Fi/MQTT가 끊기면 출력 안전 상태 유지 후 자동 재연결
```

권장 상태 머신:

```text
BOOT
  → WIFI_CONNECTING
  → MQTT_CONNECTING
  → ONLINE
       ├─ heartbeat timer
       ├─ telemetry timer
       └─ command handler → validate → execute → ACK
  → 연결 끊김
  → SAFE_RECONNECTING
  → ONLINE
```

연결 재시도 때문에 센서 읽기나 모터 제어가 멈추지 않도록 긴 `delay()`를 피하고
`millis()` 기반의 비차단 타이머를 사용한다.

---

## 7. heartbeat 구현

토픽:

```text
pfn/v1/system/heartbeat
```

발행 주기: 연결 직후 1회, 이후 5초마다 1회  
QoS: 0  
retained: false

ESP32 예제:

```json
{
  "schema_version": "pfn.v1",
  "event_id": "새 UUID",
  "trace_id": "부팅 세션용 UUID",
  "node_id": "CTRL-ESP32-ACT-01",
  "node_type": "esp32",
  "status": "online",
  "seq": 0,
  "sent_at_ms": 1789050000000,
  "uptime_ms": 5210,
  "version": "esp32-act-0.1.0"
}
```

ESP8266은 `node_type`을 `esp8266`, Octopus는 `octopus`로 보낸다.

- `sent_at_ms`: 시간이 동기화된 경우 실제 UTC epoch millisecond
- `uptime_ms`: 부팅 이후 경과시간이며 ESP32의 `millis()`를 사용 가능
- 인터넷 없이 시간이 아직 동기화되지 않았다면 거짓 날짜를 만들지 말고 시간 동기화
  여부를 직렬 로그에 표시한다. 최종 만료 명령 시험은 시간이 동기화된 상태에서 한다.

합격 기준:

- 전원을 켠 뒤 10초 이내 Pi4/UI에 `online`으로 표시
- 전원을 끈 뒤 15~17초 사이 `offline`으로 표시
- 다시 켜면 자동으로 `online` 복귀

---

## 8. telemetry 구현

토픽 형식:

```text
pfn/v1/device/{자기 node_id}/telemetry
```

예: ESP32 팬 상태

```json
{
  "schema_version": "pfn.v1",
  "event_id": "새 UUID",
  "trace_id": "현재 측정 흐름 UUID",
  "node_id": "CTRL-ESP32-ACT-01",
  "seq": 12,
  "sent_at_ms": 1789050001000,
  "uptime_ms": 6210,
  "sensor_ready": true,
  "readings": {
    "fan_level_percent": 30
  },
  "reading_sources": {
    "fan_level_percent": "FAN-01"
  }
}
```

예: ESP8266 LED와 FND 상태

```json
{
  "schema_version": "pfn.v1",
  "event_id": "새 UUID",
  "trace_id": "현재 측정 흐름 UUID",
  "node_id": "CTRL-ESP8266-LED-01",
  "seq": 7,
  "sent_at_ms": 1789050002000,
  "uptime_ms": 7210,
  "sensor_ready": true,
  "readings": {
    "light_on": true,
    "display_value": 43
  },
  "reading_sources": {
    "light_on": "LED-LIVING-01",
    "display_value": "FND-WASHER-01"
  }
}
```

### 현재 Pi4가 허용하는 센서값

| readings 이름 | 자료형 | 범위 | 허용 source 장치 ID |
| --- | --- | --- | --- |
| `sound_level_db` | 숫자 | 0~120 | `MIC-INDOOR-01` |
| `x_home_active` | boolean | true/false | `SENSOR-HOME-X-01` |
| `y_home_active` | boolean | true/false | `SENSOR-HOME-Y-01` |
| `fan_level_percent` | 숫자 | 0~100 | `FAN-01` |
| `door_position_degrees` | 숫자 | 0~180 | `SERVO-DOOR-01`, `SERVO-DOOR-02` |
| `window_position_degrees` | 숫자 | 0~180 | `SERVO-WINDOW-01` |
| `bed_curtain_position_degrees` | 숫자 | 0~180 | `SERVO-BED-CURTAIN-01` |
| `light_on` | boolean | true/false | 등록된 LED 6개 |
| `display_value` | 정수 | 0~9999 | 등록된 FND 3개 |

### 센서값 전송 시 중요한 규칙

1. `readings`의 모든 이름은 `reading_sources`에 같은 키가 있어야 한다.
2. `reading_sources` 값은 센서 또는 장치의 공식 device ID다.
3. `sensor_ready: true`라면 `readings`가 비어 있으면 안 된다.
4. 범위를 벗어나거나 자료형이 다르면 Pi4가 유효 센서값으로 반영하지 않는다.
5. 현재 등록되지 않은 센서 이름은 모두 거절된다.
6. 실제 센서가 준비되지 않았으면 `sensor_ready: false`로 보내고 임의의 정상값을 만들지 않는다.

### 새로운 센서를 추가해야 할 때

예를 들어 온도센서를 새로 달아도 바로 `temperature`를 보내면 안 된다. 먼저 팀에서
아래 네 항목을 확정해야 한다.

1. 공식 device ID: 예 `SENSOR-TEMP-01`
2. reading 이름: 예 `temperature_c`
3. 자료형과 정상 범위: 예 number, -20~80
4. 담당 controller node ID와 설치 위치

그 후 Pi4 담당자가 `device-registry.json`과 `sensor-rules.json`을 업데이트하고 검증한
버전을 배포한 다음 펌웨어에서 전송한다. 한쪽에서만 새 필드명을 만들면 안 된다.

---

## 9. command 수신과 실제 동작

ESP32가 받는 command 예제:

```json
{
  "schema_version": "pfn.v1",
  "event_id": "11111111-1111-4111-8111-111111111111",
  "trace_id": "22222222-2222-4222-8222-222222222222",
  "task_id": "33333333-3333-4333-8333-333333333333",
  "command_id": "44444444-4444-4444-8444-444444444444",
  "target_node_id": "CTRL-ESP32-ACT-01",
  "target_device_id": "SERVO-DOOR-01",
  "action": "set_position",
  "parameters": {
    "position_degrees": 90
  },
  "issued_at_ms": 1789050000000,
  "expires_at_ms": 1789050010000,
  "attempt": 1
}
```

### command 처리 순서

```text
1. JSON 파싱 가능 여부 확인
2. schema_version == pfn.v1 확인
3. target_node_id == 자기 node ID 확인
4. target_device_id가 자기 담당 장치인지 확인
5. action이 해당 장치가 지원하는 동작인지 확인
6. parameters 자료형과 범위 확인
7. expires_at_ms가 지나지 않았는지 확인
8. command_id가 이미 실행된 것인지 확인
9. 처음 받은 명령이면 실제 장치 작동
10. 센서 또는 출력 상태로 결과 확인
11. ACK 발행
```

지원해야 하는 parameter:

| action | parameter 예 | 제한 |
| --- | --- | --- |
| `set_switch` | `{ "on": true }` | boolean |
| `set_color` | `{ "color": "green" }` | off/green/red/amber |
| `set_level` | `{ "level_percent": 30 }` | 0~100 |
| `set_position` | `{ "position_degrees": 90 }` | 0~180 |
| `set_number` | `{ "value": 43 }` | 정수 0~9999 |

`night_bed_exit`의 이동 조명 명령에는 다음 값도 들어간다.

```json
{
  "on": true,
  "auto_off_after_seconds": 15
}
```

ESP8266은 LED를 켠 후 15초 뒤 스스로 꺼야 한다. 이 타이머도 긴 `delay(15000)` 대신
`millis()`로 처리하고, 그동안 heartbeat와 MQTT 처리를 계속해야 한다.

---

## 10. ACK 구현

ACK 토픽:

```text
pfn/v1/device/{자기 node_id}/ack
```

### 정상 성공 ACK

```json
{
  "schema_version": "pfn.v1",
  "event_id": "ACK마다 새 UUID",
  "trace_id": "받은 command의 trace_id 그대로",
  "command_id": "받은 command의 command_id 그대로",
  "node_id": "CTRL-ESP32-ACT-01",
  "target_device_id": "SERVO-DOOR-01",
  "ok": true,
  "duplicate": false,
  "status": "succeeded",
  "executed_at_ms": 1789050000400,
  "result": {
    "position_degrees": 90
  },
  "error": null
}
```

### 같은 command를 다시 받은 경우

실제 장치를 다시 움직이지 않는다. 저장해 둔 결과로 다음 ACK만 다시 보낸다.

```json
{
  "schema_version": "pfn.v1",
  "event_id": "새 UUID",
  "trace_id": "받은 값 그대로",
  "command_id": "받은 값 그대로",
  "node_id": "CTRL-ESP32-ACT-01",
  "target_device_id": "SERVO-DOOR-01",
  "ok": true,
  "duplicate": true,
  "status": "succeeded",
  "executed_at_ms": 1789050000800,
  "result": {
    "position_degrees": 90
  },
  "error": null
}
```

최근 실행한 command ID를 메모리의 고정 크기 목록으로 보관한다. 전원 재부팅 직후에도
위험한 동작이 반복되면 안 되는 장치는 최근 ID 일부를 NVS에도 보관하는 것을 권장한다.

### 명령이 만료된 경우

```json
{
  "schema_version": "pfn.v1",
  "event_id": "새 UUID",
  "trace_id": "받은 값 그대로",
  "command_id": "받은 값 그대로",
  "node_id": "CTRL-ESP32-ACT-01",
  "target_device_id": "SERVO-DOOR-01",
  "ok": false,
  "duplicate": false,
  "status": "rejected_expired",
  "executed_at_ms": 1789050020000,
  "result": null,
  "error": {
    "code": "COMMAND_EXPIRED",
    "message": "expires_at_ms has passed"
  }
}
```

### 잘못된 명령인 경우

`target_device_id`, action 또는 parameter가 잘못됐다면 장치를 작동시키지 않고
`ok: false`, `status: rejected_invalid`로 응답한다.

### 장치 작동에 실패한 경우

장치 ID와 명령은 정상이지만 물리 동작에 실패했다면 `status: failed`로 응답한다.

ACK는 명령을 받았다는 뜻만으로 보내면 안 된다. **실제 GPIO/서보/팬/FND 처리를 수행하고
결과를 확인한 후** ACK를 보낸다.

---

## 11. 세 시나리오에서 실제로 받아야 할 명령

### 11.1 귀가 및 자동 환경 준비 — `prepare_home`

| 순서 | 제어기 | 장치 | 동작 |
| --- | --- | --- | --- |
| 1 | `CTRL-ESP8266-LED-01` | `LED-LIVING-01` | `set_switch`, on=true |
| 2 | `CTRL-ESP32-ACT-01` | `FAN-01` | `set_level`, level_percent=30 |

여기서 `FAN-01`은 거실 팬이 아니라 **화장실 환기 팬**이다.

### 11.2 AI 취침 준비 — `prepare_sleep`

| 순서 | 제어기 | 장치 | 동작 |
| --- | --- | --- | --- |
| 1 | `CTRL-ESP8266-LED-01` | `LED-LIVING-01` | `set_switch`, on=false |
| 2 | `CTRL-ESP32-ACT-01` | `FAN-01` | `set_switch`, on=false |
| 3 | `CTRL-ESP32-ACT-01` | `SERVO-BED-CURTAIN-01` | `set_position`, 180도 |

### 11.3 야간 침대 이탈 및 이동 조명 — `night_bed_exit`

| 순서 | 제어기 | 장치 | 동작 |
| --- | --- | --- | --- |
| 1 | `CTRL-ESP8266-LED-01` | `LED-GUIDE-01` | 켜기, 15초 후 자동 끄기 |

각 command에 ACK가 와야 Pi4가 Task를 완료 상태로 바꾼다. 여러 장치 중 하나라도
최종 ACK가 없으면 Task가 `ACK_TIMEOUT`으로 실패할 수 있다.

---

## 12. 실제 연결 시험 순서

### 시험 전 준비

- [ ] Pi4와 ESP 보드 전원 전압 확인
- [ ] 모터·서보·팬 전원과 ESP 전원을 필요한 경우 분리
- [ ] 모든 전원의 GND 공통 연결 확인
- [ ] 서보·모터·팬을 ESP GPIO에서 직접 구동하지 않고 드라이버 사용
- [ ] 부팅 순간 서보·팬이 갑자기 작동하지 않는 안전 초기값 확인
- [ ] 보드에 공식 node ID 라벨 부착
- [ ] 직렬 로그에서 Wi-Fi IP, MQTT 연결 상태, 펌웨어 버전 확인

### 1단계: 단품 시험

Pi4 없이 각 장치를 직접 시험한다.

- LED: off/green/red/amber
- FND: 0, 43, 99, 9999
- 팬: OFF, 30%, 100%
- 서보: 안전 범위의 0도, 90도, 180도
- 광센서: 차단 전/후 true/false

실물 구조에 부딪히는 서보는 소프트웨어 범위가 0~180도여도 기구 안전 범위를 더
좁혀서 사용한다.

### 2단계: MQTT 연결 시험

1. Pi4를 켠다.
2. Pi4의 Wi-Fi IP와 MQTT 1884 계정을 Pi4 담당자에게 받는다.
3. ESP32를 같은 Wi-Fi에 연결한다.
4. 자기 command 토픽을 구독한다.
5. heartbeat를 보낸다.
6. UI의 기기 화면에서 해당 node가 online인지 확인한다.

PC–Pi4 직접 랜선 환경에서 Pi4 상태를 확인할 때:

```powershell
ssh root@10.10.10.2
systemctl is-active homemind-core homemind-mqtt dropbear.socket
```

세 줄 모두 `active`여야 한다.

### 3단계: telemetry 시험

1. 실제 정상값 한 건 전송
2. UI 기기 화면에 값이 표시되는지 확인
3. Pi4 SQLite에 값이 저장됐는지 확인
4. 범위를 벗어난 값 한 건을 의도적으로 전송
5. 정상 최신값으로 반영되지 않고 오류 기록만 남는지 확인
6. 자료형이 틀린 값과 등록되지 않은 이름도 같은 방식으로 확인

### 4단계: command/ACK 시험

각 액추에이터마다 다음 시험을 반복한다.

1. 정상 command 수신
2. 물리 장치가 정확히 한 번 작동
3. 3초 안에 `succeeded` ACK 전송
4. 같은 command ID 재수신
5. 물리 동작을 반복하지 않고 `duplicate: true` ACK 전송
6. 만료된 command 수신
7. 물리 동작 없이 `rejected_expired` ACK 전송
8. 잘못된 parameter 수신
9. 물리 동작 없이 `rejected_invalid` ACK 전송

### 5단계: 오프라인과 복구 시험

1. UI에서 node가 online인 것을 확인
2. ESP 보드 전원을 끈다.
3. 15~17초 후 UI에서 offline 확인
4. ESP 보드 전원을 다시 켠다.
5. 사람 조작 없이 Wi-Fi와 MQTT에 자동 재연결
6. UI에서 online 복구 확인
7. Pi4 MQTT를 5초 중단했다가 켜도 자동 재연결되는지 확인

### 6단계: 전체 시나리오 시험

다음 세 시나리오를 UI에서 차례로 실행한다.

1. `prepare_home`
2. `prepare_sleep`
3. `night_bed_exit`

각 시나리오에서 확인할 것:

- 모든 대상 장치가 맞는가
- 동작 값이 맞는가
- 실제 동작이 한 번만 발생하는가
- 각 ACK의 command ID와 trace ID가 원본과 같은가
- UI가 `실행 중 → 완료했어요`로 바뀌는가
- 실패시 UI에 원인과 `ACK_TIMEOUT` 등이 표시되는가

---

## 13. 센서 추가 후 Pi4에서 확인할 결과

Pi4는 다음 내용을 자동으로 처리한다.

- 받은 MQTT 원문 보존
- UUID 기반 중복 메시지 제거
- 허용 센서 이름, 자료형, 범위, source 장치 ID 검증
- 정상 telemetry 저장
- 명령, ACK, Task 상태 연결
- 15초 오프라인 판정
- 잘못된 Pi4 시각을 신뢰하지 않는 표시
- DB 및 저장공간 상태 확인

ESP32 담당자는 테스트 뒤 다음 결과를 Pi4/UI 담당자와 함께 확인한다.

- [ ] 해당 node가 `online`
- [ ] 마지막 heartbeat 시간이 갱신됨
- [ ] 최신 sensor reading이 실물 측정과 일치
- [ ] command가 올바른 node와 device로 전달됨
- [ ] ACK가 3초 안에 도착
- [ ] 중복 command가 물리적으로 재실행되지 않음
- [ ] 전원 차단 15초 후 `offline`
- [ ] 재부팅 후 자동 복구
- [ ] 세 시나리오 Task가 최종 `completed`

---

## 14. 제출해야 하는 결과물

ESP32 담당자는 작업이 끝나면 다음을 한 폴더로 전달한다.

1. ESP32·ESP8266·Octopus 펌웨어 전체 소스
2. 사용한 Arduino IDE 또는 PlatformIO 버전
3. 사용 라이브러리 이름과 버전
4. 보드별 최종 GPIO 핀맵
5. 장치 ID ↔ 실제 부품 ↔ GPIO 대응표
6. 보드별 펌웨어 버전
7. 정상·중복·만료·실패 ACK 직렬 로그
8. heartbeat와 telemetry 예제 JSON
9. 세 시나리오 실물 동작 영상 또는 사진
10. 아래 완료표를 채운 시험 결과

| 시험 | 결과 | 확인 시간 | 비고 |
| --- | --- | --- | --- |
| ESP32 heartbeat | 통과/실패 | | |
| ESP8266 heartbeat | 통과/실패 | | |
| 정상 telemetry | 통과/실패 | | |
| 잘못된 telemetry 거절 | 통과/실패 | | |
| 정상 command/ACK | 통과/실패 | | |
| duplicate ACK | 통과/실패 | | |
| expired ACK | 통과/실패 | | |
| 15초 offline | 통과/실패 | | |
| 재부팅 자동 복구 | 통과/실패 | | |
| `prepare_home` | 통과/실패 | | |
| `prepare_sleep` | 통과/실패 | | |
| `night_bed_exit` | 통과/실패 | | |

SSID나 비밀번호가 들어 있는 파일은 이 결과물과 Git 저장소에서 제외한다.

---

## 15. 자주 발생할 오류

### UI에서 계속 offline으로 보임

- heartbeat가 정확히 5초마다 전송되는지 확인
- `node_id` 오타 확인
- ESP32의 MQTT client ID가 다른 보드와 겹치는지 확인
- Pi4의 Wi-Fi IP와 포트 1884 확인
- MQTT 재연결 뒤 command 토픽을 다시 구독했는지 확인

### 센서 메시지는 보내지는데 UI 값이 안 바뀜

- `sensor_ready`가 true인지 확인
- `readings`와 `reading_sources`의 키가 정확히 같은지 확인
- 공식 reading 이름을 사용했는지 확인
- 값의 자료형과 범위를 확인
- source가 공식 device ID인지 확인

### Pi4가 명령을 계속 재전송함

- ACK QoS가 1인지 확인
- ACK 토픽의 node ID 확인
- `command_id`와 `trace_id`를 원본 그대로 복사했는지 확인
- ACK를 3초 안에 보내는지 확인
- MQTT 콜백 안에서 긴 모터 동작으로 통신 처리가 막히는지 확인

### 같은 명령에서 서보나 팬이 여러 번 동작함

- `attempt`가 아니라 `command_id`를 중복 판정 기준으로 사용
- 실행한 command ID 목록을 ACK 전에 기록
- 재전송 command는 실제 동작 없이 `duplicate: true` ACK만 전송

### 보드가 재연결될 때 다른 보드가 끊김

- MQTT client ID 중복이다. 각 보드와 테스트 프로그램에 서로 다른 suffix를 사용한다.

---

## 16. 변경하면 안 되는 항목

팀 합의와 Pi4 계약 업데이트 없이 다음 항목을 변경하지 않는다.

- `pfn/v1/...` MQTT 토픽
- `schema_version: pfn.v1`
- 공식 node ID와 device ID
- command와 ACK의 필드명
- 센서 readings 이름과 자료형
- ACK 3초, 3회 시도, command TTL 10초
- heartbeat 5초, offline 15초
- QoS와 retained 설정

새 센서나 장치를 추가해야 한다면 먼저 장치 ID, reading 이름, 범위, 담당 node, 위치를
합의한 뒤 Pi4 계약 파일과 ESP 펌웨어를 함께 수정한다.

---

## 17. 최종 전달 전 자체 점검

- [ ] 소스에 Wi-Fi/MQTT 비밀번호가 없음
- [ ] 모든 node ID와 device ID가 이 문서와 일치
- [ ] retained가 false
- [ ] command와 ACK가 QoS 1
- [ ] telemetry와 heartbeat가 QoS 0
- [ ] heartbeat가 5초 주기
- [ ] 모든 UUID가 올바른 형식
- [ ] 같은 command ID를 두 번 받아도 실제 동작은 한 번
- [ ] ACK의 trace ID와 command ID가 원본과 동일
- [ ] 센서값의 자료형과 범위가 정확함
- [ ] 15초 offline과 자동 복귀 확인
- [ ] 세 시나리오 실물 통합 시험 통과
- [ ] 최종 핀맵과 시험 로그 작성

이 체크가 모두 끝나면 ESP32 측 작업 완료로 판단한다.
