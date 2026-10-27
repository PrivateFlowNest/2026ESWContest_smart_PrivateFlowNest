# homemind-core 0.6.2 (2026-10-22)

0.6.1에 두 가지를 더했다: 7인치 UI 휴대폰 연결 QR용 ACL, 그리고 "상황이 달라요" 피드백으로 잘못된 조용 모드를 바로 끄는 규칙.

- 계약 `pfn/v1/mobile/pairing/request`·`pfn/v1/mobile/pairing/offer`(7인치 UI "휴대폰 연결" QR) 추가에 맞춰
  `package/mosquitto.acl`에 NODE-RPI5-01 `write request`·`read offer`, homemind-mobile-api `read request`·`write offer` 추가.
  offer에는 일회용 페어링 코드가 들어 있어 NODE-RPI5-01만 읽는다. core 계정과 다른 노드는 접근하지 못한다(core 구독 밖)
- **오탐 정정**(계약 `context-rules.json` rules_version 2 `feedback_overrides`): `pfn/v1/ui/feedback`의 `context_feedback`·`negative`가
  조용 모드가 켜져 있을 때(그룹 `quiet_mode`의 마지막 의도가 `quiet_mode_on`) 오면 core가 바로 `quiet_mode_off`(origin context_rule,
  rule `quiet_context_wrong`)를 실행하고, 그 상황이 끝날 때(`quiet_required_off`) 또는 30분이 지날 때까지 `quiet_required_on`을 무시한다.
  상황 피드백과 동작 피드백은 지금처럼 `feedback_events`에 따로 남는다. 로그: `CONTEXT_FEEDBACK_OVERRIDE`, `CONTEXT_RULE_SUPPRESSED`
- heartbeat 버전 문자열 `pi4-core-0.6.2`, control·serviceinfo 0.6.2

ACL 템플릿은 설치 때 `/data/share/homemind/config/mosquitto.acl`이 없을 때만 복사된다. 0.6.0·0.6.1을 이미 설치해 ACL 파일이
있는 기기는 `package-defaults/mosquitto.acl`을 사람이 확인 후 덮어써야 새 토픽이 열린다(지금 Pi4는 0.5.2라 해당 없음).

## 시험 (PC, 시뮬레이션 — Pi4 실기 아님)
| 시험 | 결과 |
|---|---|
| `tests/run-pc-docker-test.sh 30` (x86) | PC_DOCKER_TEST_PASS (빌드, registry 검사, `--password` 거부, selftest, v5 마이그레이션, soak) |
| selftest 오탐 정정 | 부정 피드백 → `quiet_mode_off` 1번, 같은 회의 다시 → 무시, 회의 끝 → 추가 off 없음, 다음 회의 → 다시 on (DB 검사 4개) |
| PC 데모 스택(core + 가짜 ESP) | 같은 흐름이 실제 브로커·가짜 ESP ACK까지 완료 |
| `tests/run-acl-test.sh` (실제 Mosquitto) | ACL_TEST_PASS, 22개(새 6개: UI → Mobile API 요청, offer는 UI만, core·RPI5-02 차단, 다른 계정의 offer 위조 차단, ESP 요청 차단) |
| IPK를 arm64 Ubuntu에 풀어 core + selftest (QEMU) | SELFTEST_PASS, CORE_STARTED |

IPK `dist-ipk/homemind-core_0.6.2_aarch64.ipk` 980,912 bytes, SHA-256 `7621c4bf…`. 0.6.1 IPK는 그대로 둔다.
설치는 사용자 승인 후 `deploy/NODE-RPI4-01/install.sh`.
