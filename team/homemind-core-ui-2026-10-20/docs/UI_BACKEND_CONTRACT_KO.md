# HomeMind UI–Pi4 공통 계약

이 문서는 UI 상세 설계서, 공용 ID 정의표, Pi4의 `pfn.v1` MQTT 계약을 하나로
연결한다. 화면 디자인은 바뀔 수 있지만 여기 적힌 ID, 상태값, 토픽과 JSON 필드는
Pi4·Flutter·ESP32가 동시에 합의하기 전에는 한쪽에서만 변경하지 않는다.

## 1. 구현 경계

- Pi4 Core가 Task, 기기 온라인 상태, 최신 Context, 안전·개인정보 상태의 기준이다.
- Flutter는 정책을 다시 판단하지 않고 Pi4가 보낸 상태를 표시하고 사용자 행동을 전달한다.
- ESP32·ESP8266·Octopus는 자신에게 연결된 실제 장치를 구동하고 ACK/telemetry를 보낸다.
- 카메라 영상 자체는 MQTT로 보내지 않는다. MQTT에는 감지 결과와 스트림 상태만 보낸다.
- 개발 중 Flutter는 `FakeDataSource`와 `MqttDataSource`를 사용한다. Pi5 배포 때 필요하면
  동일한 모델을 반환하는 `DbusDataSource`를 추가한다.

## 2. ID 사용법

ID의 원본은 `contracts/device-registry.json`이다.

| 구분 | 예 | 사용하는 곳 |
| --- | --- | --- |
| 노드 ID | `CTRL-ESP32-ACT-01` | MQTT 토픽, `node_id`, `target_node_id` |
| 실제 장치 ID | `SERVO-DOOR-01` | `target_device_id`, 센서 reading 출처 |
| MQTT client ID | `CTRL-ESP32-ACT-01-runtime` | 브로커 접속 이름. 동시에 접속하는 프로세스마다 고유 |
| 메시지 ID | UUID | 매 발행 메시지의 `event_id` |
| 흐름 ID | UUID | 한 시나리오 전체에서 유지하는 `trace_id` |
| 명령 ID | UUID | 명령과 ACK를 연결하는 `command_id` |

`공용 ID`는 Pi4와 ESP32가 같은 ID를 쓰는 뜻이 아니다. 모든 프로그램이 같은
등록표를 보고 각 물리 장치를 동일한 이름으로 부른다는 뜻이다.

예를 들어 Pi4가 현관문을 열 때:

```text
토픽: pfn/v1/device/CTRL-ESP32-ACT-01/command
target_node_id: CTRL-ESP32-ACT-01
target_device_id: SERVO-DOOR-01
action: set_position
```

ACK는 `pfn/v1/device/CTRL-ESP32-ACT-01/ack`로 돌아오며 같은 `command_id`,
`trace_id`, `target_device_id`를 포함한다.

## 3. MVP 화면별 데이터와 행동

### 01. Pi5 홈 대시보드

- 표시: 연결 상태, 현재 Context·신뢰도, 활성/확인 대기 Task, P0 안전 상태, 갱신 시각
- 최초 데이터: `state_request` → `state_response/{client_id}`
- 실시간 갱신: `task_update`, `system_health`, `safety_alert`, `privacy_state`
- 행동: Task 승인·거절, Context/Task/시스템 상세 이동
- snapshot의 `clock.valid=false`이면 날짜를 현재 시각처럼 표시하지 않고 `시간 미확인`으로 표시한다.

### 02. Task 목록

- 표시: Task ID, 제목용 intent, 대상 장치, 상태, 우선순위, 안전 수준, revision, 시각
- 데이터: snapshot의 `tasks` + `task_update`
- 행동: `approve`, `reject`, `cancel`, `resume`
- UI는 행동을 보낼 때 보고 있던 `expected_revision`과 `action_token`을 함께 보낸다.
- `failure.code=ACK_TIMEOUT`이면 같은 명령을 세 번 시도했지만 장치 응답이 없었다는 뜻으로 표시한다.

### 03. Context 상세

- 표시: zone, observation type, confidence, 근거 attributes, 관측 시각, 추천 Task
- 데이터: snapshot의 `world_state`
- 행동: Context 수정과 `context_feedback` 제출

### 04. 기기

- 표시: 노드/장치 ID, 이름, 위치, online/degraded/offline, 마지막 수신 시각,
  최근 telemetry, 허용 capability
- 데이터: snapshot의 `devices`, `device-registry.json`, `system_health`
- 행동: 허용된 수동 제어만 `device_command`로 발행

### 05. 개인정보

- 표시: 개인정보 모드, 카메라·마이크·센서 수집 허용, 로컬 처리 여부, 녹화 여부
- 데이터: snapshot의 `privacy` + `privacy_state`
- 행동: `privacy_set`; 카메라/마이크 물리 표시와 실제 활성 상태가 일치해야 한다.

### 06. QR 페어링

- MVP 후순위. 일회용 token 발급·만료·재사용 방지는 MQTT가 아닌 로컬 pairing API로 구현한다.
- UI 목업의 QR 문자열을 실제 인증 정보로 사용하지 않는다.

### 07. P0 안전 알림

- 표시: alert ID, 종류, 위치, 심각도, 근거, 발생 시각, 자동 조치 결과
- 데이터: `safety_alert`
- 행동: `safety_ack`; 사용자가 확인하기 전에는 전체 화면을 자동으로 닫지 않는다.
- 동일 `alert_id` ACK 재전송은 안전하게 중복 처리한다.

### 08. 오프라인 안전 모드

- 표시: 마지막 정상 snapshot, 마지막 갱신 시각, 끊긴 구성요소, 계속 가능한 로컬 기능
- 데이터: `system_health`, heartbeat 15초 오프라인 판정, 캐시된 snapshot
- 행동: snapshot 재요청과 재연결. 연결이 끊겼다고 과거 값을 현재 값처럼 표시하지 않는다.
- Pi4 연결이 살아 있어도 `clock.valid=false`이면 데이터는 유지하되 날짜는 신뢰하지 않는다는 경고를 표시한다.

### 09. Task 상세

- 표시: 생성 이유, 영향, 상태 이력, 대상 장치, 실행 결과, 오류
- 데이터: snapshot 또는 최신 `task_update`
- 행동: 승인·거절·취소·재개. `expected_revision` 불일치 시 새 snapshot을 요청한다.

### 10. Pi5 카메라

- 영상: 로컬 camera stream API
- 상태: snapshot의 camera device와 `privacy_state`/`system_health`
- MQTT에는 프레임을 넣지 않고 `CAM-INDOOR-01`, `CAM-OUTDOOR-01` 감지 결과만 보낸다.

### 11. 피드백

- `context_feedback`: AI가 상황을 맞게 이해했는지
- `action_feedback`: 실제 동작이 도움이 되었는지
- 응답하지 않음은 `unknown`이며 부정 평가로 바꾸지 않는다.

### 12. 시스템·성능

- 표시: Pi4 Core, Pi5 UI, MQTT, 장치 상태, DB 용량, 디스크 사용률, 지연 시간, Pi4 시간 신뢰 여부
- 데이터: `system_health` + snapshot의 `clock`
- 실제 임계값: 저장공간 70% 경고, 장치 15초 오프라인
- `clock`은 `valid`, `boot_id`, `uptime_ms`, `became_valid_at_ms`를 제공한다. `valid=false`일 때 원본 데이터는 삭제하지 않는다.

### 13–15. 모바일

- Pi5 화면과 같은 Task/Context/안전 모델을 재사용한다.
- 외부 접속 인증과 원격 camera stream은 현재 로컬 MVP 이후 구현한다.
- 위험한 명령은 상세 영향 설명과 재확인을 거친다.

## 4. UI 상태 매핑

백엔드 내부 상태와 UI 문구는 다음처럼 고정한다.

| 백엔드 상태 | UI 문구 |
| --- | --- |
| `proposed` | 이런 작업을 제안해요 / 확인이 필요해요 |
| `ready` | 실행 준비 중 |
| `running` | 실행 중 |
| `deferred` | 조용히 보류했어요 |
| `completed` | 완료했어요 |
| `failed` | 실행하지 못했어요 |
| `rejected` | 거절했어요 |
| `cancelled` | 취소했어요 |

UI 설계서의 `DETECTED`, `PREPARING`, `AWAITING_CONFIRMATION`, `ACTIVE`,
`RESUME_READY`는 별도의 DB 상태로 늘리지 않고 다음처럼 표현한다.

- `DETECTED`: perception/world_state 단계
- `PREPARING`: `ready`
- `AWAITING_CONFIRMATION`: `proposed`
- `ACTIVE`: `running`
- `RESUME_READY`: `deferred`이면서 `resume_allowed=true`

## 5. 전송 규칙

- 모든 메시지: `schema_version=pfn.v1`, `event_id`, `trace_id`, UTC epoch `_ms`
- Task·명령·ACK·UI 행동·안전·개인정보: QoS 1, retained false
- telemetry·heartbeat·system health: QoS 0, retained false
- 명령 제한시간 3초, 최대 3회, 전체 유효시간 10초
- 재시도할 때 `command_id`와 `trace_id`는 유지하고 `attempt`만 증가한다.
- 세 번째 시도 후에도 ACK가 없으면 Task는 `failed`, 실패 코드는 `ACK_TIMEOUT`이 된다.
- ESP32는 같은 `command_id`를 다시 받으면 실제 동작을 반복하지 않고
  `duplicate=true` ACK를 보낸다.
- 장치 토픽의 `{node_id}`와 JSON의 `target_node_id` 또는 `node_id`는 반드시 같다.
- 한 메시지는 256 KiB를 넘지 않는다.

## 6. 우선 구현 순서

1. snapshot을 Flutter 모델로 변환
2. Task 목록과 `task_update` 실시간 반영
3. 승인·거절·취소·재개와 revision 충돌 처리
4. `CTRL-ESP32-ACT-01` 대상 명령·ACK 왕복
5. 15초 오프라인 UI
6. P0 `safety_alert`·`safety_ack`
7. 개인정보 상태
8. 카메라 스트림·QR·모바일 기능

이 순서에서는 화면 하나를 만들 때마다 `Flutter → Pi4 → 가짜 ESP32 → ACK → Flutter`
전체 경로를 시험한다.

## 7. 현재 발견된 불일치와 결정 필요 항목

| 항목 | 현재 상태 | 연결 전 조치 |
| --- | --- | --- |
| Pi4 | 현재 개발 장치를 `NODE-RPI4-01`/`10.10.10.2`로 지정 | 두 번째 Pi4를 연결할 때 실물 라벨 확인 |
| 서보 모델 | ID 원본은 `BG90S`, 기존 BOM은 `MG90S` | 실제 모터 각인 확인 후 표기 하나로 통일 |
| 카메라·마이크 | `NODE-RPI5-01` 담당으로 등록 완료 | Pi5 실물 연결 뒤 장치 인식 시험 |
| 이동 조명 | `LED-GUIDE-01`로 등록 완료 | ESP32 실제 배선 핀 확정 |
| 거실 조명 | `LED-LIVING-01`로 등록 완료 | ESP32 실제 배선 핀 확정 |
| 침대 커튼 | `SERVO-BED-CURTAIN-01`로 별도 등록 완료 | ESP32 실제 배선 핀·각도 보정 |
| FND | 세탁기·에어컨·TV 3개 공식 ID 등록 완료 | 실제 담당 제어기와 핀 확정 |
| `resume` | 상태 전환과 revision 검증 구현·시험 완료 | Flutter 실제 연결 |
| P0·개인정보 | Pi4 저장·발행·자체시험 완료 | Flutter 화면 실제 연결 |

공식 ID는 확정됐지만 ESP32의 실제 GPIO 핀과 FND 담당 제어기는 하드웨어 배선표에서
추가 확정한다. 등록표에 없는 임시 ID는 실장비 모드에서 계속 차단한다.
