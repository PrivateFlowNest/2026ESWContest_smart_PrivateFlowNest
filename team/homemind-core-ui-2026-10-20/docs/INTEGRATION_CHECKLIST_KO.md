# Pi5·ESP32 연결 체크리스트

## 확정된 공통 규칙

- 스키마 버전: `pfn.v1`
- 명령 ACK 제한시간: 3초
- 최대 명령 시도: 3회(최초 1회 + 재시도 2회)
- 명령 유효시간: 10초
- heartbeat 간격: 5초
- 오프라인 판단: 마지막 메시지 이후 15초
- 모든 `_ms` 시간: UTC Unix epoch millisecond
- 명령과 ACK: QoS 1
- telemetry와 heartbeat: QoS 0
- 모든 메시지: retained `false`

## 토픽

| 토픽 | 보내는 장치 | 받는 장치 |
| --- | --- | --- |
| `pfn/v1/device/{node_id}/command` | Pi4 | ESP32 |
| `pfn/v1/device/{node_id}/ack` | ESP32 | Pi4 |
| `pfn/v1/device/{node_id}/telemetry` | ESP32 | Pi4 |
| `pfn/v1/system/heartbeat` | 모든 장치 | Pi4/UI |
| `pfn/v1/system/health` | Pi4 | Pi5/UI |
| `pfn/v1/system/safety/alert` | Pi4 | Pi5/UI |
| `pfn/v1/ui/safety/ack` | Pi5 UI | Pi4 |
| `pfn/v1/ui/privacy/set` | Pi5 UI | Pi4 |
| `pfn/v1/system/privacy/state` | Pi4 | Pi5/UI |
| `pfn/v1/ui/feedback` | Pi5 UI | Pi4 |
| `pfn/v1/ai/perception` | Pi5 | Pi4 |
| `pfn/v1/core/task/request` | Pi5 | Pi4 |
| `pfn/v1/core/task/update` | Pi4 | Pi5/UI |
| `pfn/v1/ui/action` | Pi5 UI | Pi4 |
| `pfn/v1/core/state/request` | Pi5 UI | Pi4 |
| `pfn/v1/core/state/response/{client_id}` | Pi4 | 요청한 Pi5 UI |

Pi4 코어는 `pfn/v1/#` 전체를 구독하므로 이후 추가되는 버전 1 토픽도 원문을
잃지 않고 `mqtt_events`에 저장한다.

## 각 장치에서 반드시 지킬 사항

1. `node_id`는 장치마다 영구적으로 다르게 지정한다.
2. MQTT `client_id`도 동시에 연결되는 장치끼리 절대 같으면 안 된다.
3. `event_id`, `command_id`, `trace_id`는 UUID 형식을 사용한다.
4. 같은 명령을 재시도할 때 `command_id`와 `trace_id`를 바꾸지 않는다.
5. ESP32는 이미 실행한 `command_id`를 다시 받으면 동작을 반복하지 않고
   `duplicate: true` ACK만 보낸다.
6. ACK의 `command_id`와 `trace_id`는 받은 명령과 정확히 같아야 한다.
7. ESP32 재부팅 후 `seq`가 0으로 돌아가도 괜찮다. 중복 기준은 `event_id`다.
8. 실제 센서를 연결하면 `sensor_ready: true`와 `readings` 객체를 사용한다.
9. Wi-Fi·MQTT 비밀번호는 코드, JSON, Git, SQLite에 넣지 않는다.
10. 한 메시지는 256 KiB를 넘기지 않는다.
11. 공용 ID의 원본은 `contracts/device-registry.json`이며 노드 ID와 실제 장치 ID를
    혼용하지 않는다.
12. 실제 장치 명령은 토픽의 제어기 `node_id`와 JSON의 `target_node_id`가 같아야 하고,
    `target_device_id`는 그 제어기에 등록된 장치여야 한다.
13. 카메라 프레임과 음성 원본은 MQTT로 보내지 않는다.

## 네트워크 연결

- PC–Pi4 직접 랜선 개발: `10.10.10.2:1883`, 격리된 개발용 연결
- 랩탑 Flutter 웹 미리보기: `ws://10.10.10.2:9001`, 직접 랜선 개발 전용
- ESP32·Pi5 Wi-Fi: `Pi4의 해당 Wi-Fi IP:1884`, 사용자 이름과 비밀번호 필수
- Wi-Fi가 바뀌면 Pi4의 Wi-Fi IP는 바뀔 수 있으므로 장치 설정도 갱신하거나
  추후 고정 이름(mDNS/DNS)을 도입한다.

## 실장비를 다시 모았을 때의 시험 순서

1. Pi4의 세 서비스가 `active`인지 확인한다.
2. ESP32 heartbeat가 `devices`에 `online`으로 저장되는지 확인한다.
3. 실제 센서 telemetry 한 건이 저장되는지 확인한다.
4. LED ON 명령 한 건을 보내 일반 ACK를 확인한다.
5. 같은 `command_id`로 다시 보내 `duplicate: true` ACK를 확인한다.
6. 만료된 명령으로 `rejected_expired` ACK를 확인한다.
7. ESP32 전원을 꺼 15초 뒤 `offline`으로 바뀌는지 확인한다.
8. Pi5에서 시작한 요청의 `trace_id`가 Pi4 명령과 ESP32 ACK까지 유지되는지 확인한다.
9. Pi4를 재부팅하고 모든 서비스와 기존 DB 기록이 유지되는지 확인한다.

## Pi5·UI 담당자에게 전달할 파일

- `contracts/protocol.json`: 모든 토픽, QoS, 시간 제한
- `contracts/device-registry.json`: Pi·제어기·센서·액추에이터 공용 ID
- `docs/UI_BACKEND_CONTRACT_KO.md`: 화면별 필요한 데이터와 행동 매핑
- `contracts/perception.schema.json`: Pi5 감지 결과
- `contracts/task-request.schema.json`: Pi5 작업 제안
- `contracts/task-update.schema.json`: Pi4 작업 상태 알림
- `contracts/ui-action.schema.json`: 사용자 승인·거절·취소
- `contracts/state-request.schema.json`: 앱의 전체 상태 요청
- `contracts/state-response.schema.json`: Pi4의 전체 상태 응답

TaskFlow 공식 토픽과 JSON 형식은 위 파일로 확정했다. 팀원은 임시 토픽이나
자체 필드명을 새로 만들지 말고, 변경이 필요하면 계약 파일을 먼저 함께 수정한다.
