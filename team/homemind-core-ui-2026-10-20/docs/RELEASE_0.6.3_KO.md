# homemind-core 0.6.3 (2026-10-22)

0.6.2에 **휴대폰 승인**을 더했다. 휴대폰 앱(Mobile API 0.5)이 승인 대기 작업을 승인·거절할 수 있고,
현관문 열기만은 집 안 7인치 화면에서만 승인된다.

- 계약: 새 토픽 `pfn/v1/mobile/task/action`(문서는 `ui-action.schema.json` 그대로), `task-policy.json` policy_version 2:
  규칙 필드 `phone_may_approve`(없으면 true), `entrance_door_open_needs_approval`에 `false`.
  `validate-contracts.py`가 "현관문 열기는 휴대폰 승인 거부, 나머지 대기 작업은 허용"을 고정 검사한다
- core가 `pfn/v1/mobile/task/action`을 구독해 UI 동작과 같은 경로로 처리하되, 휴대폰에서 온 approve·resume은 작업이 처음
  들어올 때 고른 정책 규칙을 다시 보고 `phone_may_approve: false`면 거부한다(`user_feedback.error_text`
  `phone_approval_not_allowed`, 로그 `PHONE_APPROVAL_REFUSED`). 거절·취소는 허용. 작업 상태·리비전·토큰은 그대로
- `package/mosquitto.acl`: homemind-mobile-api `write pfn/v1/mobile/task/action`, NODE-RPI4-01(core) `read`.
  Mobile API 계정은 `pfn/v1/ui/#`에 쓸 수 없어 7인치 UI로 위장하지 못한다
- heartbeat 버전 문자열 `pi4-core-0.6.3`, control 0.6.3

0.6.2와 같이 ACL 템플릿은 `/data/share/homemind/config/mosquitto.acl`이 없을 때만 복사된다(지금 Pi4는 0.5.2라 해당 없음).

## 시험 (PC, 시뮬레이션 — Pi4 실기 아님)
| 시험 | 결과 |
|---|---|
| `contracts/validate-contracts.py` + 변형 시험 | PASS, 40개(새 4개: 현관문 휴대폰 승인 허용·자동 규칙에 플래그·다른 스키마·core 구독 누락은 실패) |
| `tests/run-pc-docker-test.sh 20` (x86) | PC_DOCKER_TEST_PASS. selftest 새 경로: 휴대폰 현관문 승인 → 거부(리비전 그대로), 휴대폰 거절 → rejected, 휴대폰 LLM 제안 승인 → completed (DB 검사 3개) |
| `tests/run-acl-test.sh` (실제 Mosquitto) | ACL_TEST_PASS, 26개(새 4개: 휴대폰 승인 → core, Mobile API가 `pfn/v1/ui/action` 쓰기 차단, 다른 계정의 휴대폰 승인 위조 차단, UI 승인 → core) |
| PC 데모 스택(core + 가짜 ESP + C++ Mobile API) | 23/23: LLM 제안 휴대폰 승인 → 완료, 현관문 휴대폰 승인 403, 버스에 직접 보낸 위조 휴대폰 승인도 core가 거부, 휴대폰 거절 → rejected, SSE 이벤트 |
| IPK를 arm64 Ubuntu에 풀어 core + selftest (QEMU) | SELFTEST_PASS, CORE_STARTED (aarch64, e_machine 0xb7) |

IPK `dist-ipk/homemind-core_0.6.3_aarch64.ipk` 986,066 bytes, SHA-256 `7d3475cc…`. 0.6.2 IPK는 그대로 둔다.
설치는 사용자 승인 후 `deploy/NODE-RPI4-01/install.sh`(그 다음 `install-mobile-api.sh`, Mobile API는 core 0.6.3 이상 필요).
