# HomeMind 계약 (정본)

이 폴더가 HomeMind 계약의 **유일한 정본**이다. 2026-10-21 P1에서 팀원 `team/homemind-core-ui-2026-10-20/contracts`(pfn.v1)를 여기로 옮겼고, 다른 곳은 이 폴더를 참조만 한다.

```
contracts/
  validate-contracts.py   검증기 (의존성 없음). 팀 폴더 tests/validate-contracts.py는 이걸 부르는 래퍼
  tests/                  검증기 변이 시험 (python -m unittest discover -s contracts/tests)
  pfn.v1/                 노드 사이 MQTT 계약 (토픽·필드·QoS·장치 ID)
    examples/             모든 메시지 예제. 검증기가 전부 스키마·교차 검사
  rpi5-internal/          RPI5 노드 안에서만 쓰는 AI 이벤트 (ISO 시간, 노드 밖 전송 금지)
    examples/
    legacy/               대체된 계약 (pi4-task-proposal-envelope: quiet 전이 HTTP 제안. perception ACTIVITY_CONTEXT → core 시나리오로 대체)
```

## 변경 규칙

1. 공식 장치 ID, 기존 MQTT 토픽, 기존 필드는 바꾸지 않는다. 추가만 한다
2. 계약을 먼저 고치고 `python contracts/validate-contracts.py`가 `CONTRACT_VALIDATION_PASS`인지 확인한 뒤 코드를 고친다
3. 새 메시지에는 `examples/` 예제를 넣고 검증기의 예제 표(`PFN_EXAMPLES`, `INTERNAL_EXAMPLES`)에 등록한다. 표에 없는 예제 파일은 검증 실패
4. `registry_version`, `rules_version`, `map_version`을 올린다

## pfn.v1 요약

| 파일 | 내용 |
|---|---|
| `protocol.json` | 토픽, 메시지 스키마 매핑, QoS·retained 정책, `topic_policies`(발행·구독 노드, ACL 계정, core 구독 여부), `task_intents` |
| `device-registry.json` (v3) | 노드·장치 ID, 담당 제어기, 핀·채널(전장 확정안), `wiring_status`, zone 목록, 서보 `positions`, LED `supported_colors` |
| `scenario-rules.json` (v2) | core 시나리오 intent → 명령 목록 |
| `sensor-rules.json` (v2) | telemetry reading 이름·형식·범위·출처 장치 |
| `ai-action-map.json` (v1) | AI 어휘(EXAONE·파서) → registry 장치·core 명령, AI zone → registry zone |
| `device-control.schema.json` | intent `device_control`의 parameters |
| `llm-request.schema.json` / `llm-result.schema.json` | 파서가 못 푼 음성 문장 → EXAONE, 결과 |
| `perception.schema.json` | `FIGURE_PRESENCE`, `ACTIVITY_CONTEXT`, `PARTIAL_INTENT_HINT`별 attributes |
| `task-policy.json` (v2) | 출처(`parameters.origin`)별 허용 발신 노드, 바로 실행/UI 승인 규칙(첫 일치), 휴대폰 승인 허용(`phone_may_approve`), 거절 코드 |
| `context-rules.json` (v1) | perception → core 시나리오 intent (ACTIVITY_CONTEXT quiet → `quiet_mode_on/off`) |

### 음성·상황 경로

```
RPI5-02 Whisper final ─ 결정론적 파서 ─┬─ 해석됨 → core/task/request (device_control 또는 시나리오 intent)
                                        └─ 못 풂 → ai/llm/request (원문 전사, RPI5 계정만, expires_at_ms 필수)
RPI5-01 EXAONE → 가드 → ai-action-map → core/task/request, 결과는 ai/llm/result (원문 없음)
RPI5-02 context-fusion → ai/perception ACTIVITY_CONTEXT (quiet_required) → core 규칙 → quiet_mode_on/off
RPI4-01 core: 모든 task/request를 registry로 검증 → command → ESP → ACK
```

- `ai/llm/*`는 core가 구독·저장하지 않는다(`topic_policies.*.core_subscribes=false`). 브로커 ACL은 P3에서 적용
- `device_control`: core는 `target_node_id`를 registry에서 정하고 요청 값을 믿지 않는다. `wiring_status=wired`, capability, `command.schema.json` 파라미터, LED 색, 서보 등록 각도를 확인한다
- `origin`: `voice_parser`는 NODE-RPI5-02, `llm`은 NODE-RPI5-01에서만 온다

### 승인 정책 (2026-10-21 사용자 결정, `task-policy.json`)

| 요청 | 처리 |
|---|---|
| 음성 파서(RPI5-02)·UI/휴대폰(RPI5-01, RPI4-01)의 `device_control` | 바로 실행 |
| 현관문(SERVO-DOOR-01) 열기 | UI 승인 (출처 무관) |
| LLM(RPI5-01) 제안 | UI 승인 |
| `quiet_mode_on/off` (음성·UI·상황 규칙) | 바로 실행 |
| `prepare_home`·`prepare_sleep`·`night_bed_exit`, origin 없는 요청 | UI 승인 |
| (2026-10-22, v2) 승인 대기 작업을 휴대폰에서 | `pfn/v1/mobile/task/action`으로 승인·거절 가능. 단 `phone_may_approve: false` 규칙(현관문 열기)은 휴대폰 승인·재개 거부, 거절은 가능 |
| 출처와 발신 노드 불일치, 배선 안 됨, capability 밖, 미등록 각도, 미지원 색 | `rejected` + `failure.code` |

core 구독 목록은 `protocol.json core_subscriptions`(LLM 토픽 제외). 브로커 ACL 템플릿은 `team/homemind-core-ui-2026-10-20/package/mosquitto.acl`.

### 2026-10-21 사용자 결정 반영

| [미정] 항목 | 결정 | 계약 반영 |
|---|---|---|
| 서보 열림·닫힘 각도 | 임시값 닫힘 0°, 열림 90°, 모형 보정 후 교체 → **registry v4 (2026-10-25)**: 현관문·창문은 열기/닫기 모터, 90 = 열림 / 0 = 닫힘은 코드(ESP32가 모터 값으로 바꿈), 화장실 문 철거 | registry `positions` + `calibrated: true`, SERVO-DOOR-02 `not_wired`, ai-action-map BATHROOM_DOOR 끔 |
| 팬 PWM | 실기 확인 전까지 ON/OFF | FAN-01 capability `set_switch`만, `prepare_home`은 `set_switch on`, SET_LEVEL 매핑 비활성 |
| LED 단색 표현 | 점등 = 전원, 느린 깜빡임 = 조용·음소거·알림 보류 | `set_color`에 `pattern`(`steady`/`blink`) 추가, `quiet_mode_on/off` 시나리오, telemetry `light_pattern` |

### 펌웨어가 지킬 LED 규칙

- `set_switch {on}`: 전원 상태. 깜빡임 오버레이가 없으면 on=점등, off=소등
- `set_color {color:"red", pattern:"blink"}`: 조용 오버레이 켬(전원 상태와 무관하게 느린 깜빡임)
- `set_color {color:"red", pattern:"steady"}` 또는 `pattern` 없음: 오버레이 끔, 전원 상태 표시로 복귀
- `set_color {color:"off"}`: 오버레이 끔 + 소등
- R 단색만 배선됨. `supported_colors` 밖의 색은 core가 거절

## rpi5-internal 요약

- 모든 이벤트에 `source_node_id` 필수. pi5.* 출처는 `NODE-RPI5-02`, 시나리오 재생(`pc.scenario`)은 `PC-SIMULATION`이며 `simulated: true`
- 전사(`speech-transcript`) scope `RPI5_ONLY`: RPI5-02 안에서만 쓰고, 밖으로는 `pfn/v1/ai/llm/request`로 RPI5-01에만
- 마이크는 방향 기능이 없으므로 `SOURCE_DIRECTION` 특징은 없다

## 검증

```
python contracts/validate-contracts.py
python -m unittest discover -s contracts/tests
```
