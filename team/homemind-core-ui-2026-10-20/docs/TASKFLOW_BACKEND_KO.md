# HomeMind Pi4 TaskFlow 백엔드 연결 설명서

## 각 장치의 역할

- Pi5: 카메라·음성·AI로 상황을 파악하고 작업 후보를 제안한다.
- Pi4: 집의 최신 상태와 작업 기록을 보관하고, 사용자 결정을 반영한다.
- ESP32: 센서값을 보내고 LED·모터 같은 실제 장치를 작동한 뒤 ACK를 보낸다.
- Pi5 앱: Pi4가 보낸 작업과 집 상태를 보여주고 승인·거절·취소를 전달한다.

Pi4는 판단 기록의 기준점이다. Pi5나 앱이 재시작해도 Pi4의 SQLite를 다시 조회하면
현재 작업과 장치 상태를 복구할 수 있다.

## 가장 먼저 연결할 흐름

```text
Pi5 감지 결과
  -> pfn/v1/ai/perception
  -> Pi4 world_state 저장
  -> pfn/v1/core/task/request
  -> Pi4 tasks에 proposed 저장
  -> pfn/v1/core/task/update
  -> 앱에 승인 카드 표시
  -> pfn/v1/ui/action (approve)
  -> Pi4 tasks를 ready로 변경, revision 증가
  -> pfn/v1/core/task/update
  -> 앱 화면 갱신
```

실제 장치 동작은 `ready` 이후 Pi4가 ESP32 명령으로 변환하는 다음 단계에서 붙인다.
ESP32의 ACK가 성공이면 작업을 `completed`, 실패하면 `failed`로 바꾸도록 확장한다.

## 작업 상태

| 상태 | 쉬운 의미 |
| --- | --- |
| `proposed` | AI가 제안했고 사용자 선택을 기다림 |
| `ready` | 사용자가 승인하여 실행 가능 |
| `running` | 실제 실행 중 |
| `deferred` | 더 중요한 일 때문에 잠시 보류 |
| `completed` | 정상 완료 |
| `failed` | 실행 실패 |
| `rejected` | 사용자가 거절 |
| `cancelled` | 시작 전 또는 실행 중 취소 |

한 작업이 바뀔 때마다 `revision`을 1씩 올린다. 앱이 오래된 revision으로 행동하면
잘못된 화면에서 누른 요청으로 판단할 수 있다.

## 팀 공통 규칙

1. `event_id`, `trace_id`, `task_id`, `command_id`는 UUID를 사용한다.
   노드·센서·액추에이터 ID는 `contracts/device-registry.json`의 고정 ID를 사용한다.
2. 하나의 동작 흐름은 처음부터 끝까지 같은 `trace_id`를 유지한다.
3. 같은 메시지를 다시 보낼 때 `event_id`를 바꾸지 않는다.
4. 모든 시간은 UTC Unix epoch millisecond인 `_ms` 정수로 보낸다.
5. TaskFlow 토픽은 QoS 1, retained false를 사용한다.
6. `client_id`는 동시에 실행되는 프로그램마다 다르게 지정한다.
7. JSON 필드 변경은 `contracts/`의 스키마를 먼저 수정한 뒤 전 장치에 함께 반영한다.
8. MQTT 토픽의 제어기 ID와 명령의 `target_node_id`는 같아야 하며,
   `target_device_id`는 해당 제어기에 연결된 실제 장치여야 한다.

## 앱 시작 시 동기화

앱은 MQTT 연결 직후 `pfn/v1/core/state/request`를 한 번 보낸다. Pi4는
`pfn/v1/core/state/response/{client_id}`로 다음 세 묶음을 반환한다.

- `tasks`: 최신 작업 목록
- `devices`: ESP32·Pi5 등의 online/offline 상태
- `world_state`: 위치별 사람 유무 등 최신 판단

이후에는 `pfn/v1/core/task/update`를 구독해 바뀐 작업만 실시간 반영한다. 앱이
재연결되거나 순서를 놓쳤다고 의심되면 snapshot을 다시 요청한다.

## 현재 자동 시험이 확인하는 것

- 센서 telemetry와 heartbeat 저장
- ESP32 명령 ACK와 같은 명령의 중복 실행 방지
- 잘못된 JSON도 원문 보존
- Pi5 perception 저장과 world state 갱신
- Pi5 작업 제안의 `proposed` 저장
- UI 승인 후 `ready`, revision 2 전환
- 작업 전환 이력과 사용자 피드백 저장
- 앱 상태 snapshot 응답

시험 마지막에 `SELFTEST_PASS`가 나오면 위 항목이 모두 통과한 것이다.

## 아직 실장비를 모아야 가능한 것

- Pi5의 실제 카메라 인식 결과 연결
- Flutter 앱의 MQTT/D-Bus 브리지 연결
- `ready` 작업을 실제 ESP32 명령으로 변환하는 시나리오별 매핑
- ESP32 ACK 결과에 따른 `completed` 또는 `failed` 전환
- 여러 작업이 동시에 올 때 P0/P1/P2 우선순위와 보류·재개 시험
