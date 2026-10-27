# homemind-core 0.6.4 (2026-10-24)

0.6.3에 **승인 대기 만료**를 더했다. 아무도 답하지 않은 승인 대기 작업은 정해진 시간이 지나면 core가 닫는다.
그전에는 `proposed` 작업이 승인·거절 전까지 영원히 남아, 며칠 지난 AI 제안이나 현관문 열기 요청을 뒤늦게 승인해도
그대로 실행됐다(PC 데모 DB에 10-22 시험이 남긴 대기 작업 100건이 휴대폰 앱에 그대로 보여 발견).

- 계약: `task-policy.json` policy_version 3(사용자 결정 2026-10-24). 최상위 `proposal_ttl_seconds: 600`(기본 10분),
  규칙별 `proposal_ttl_seconds`(`entrance_door_open_needs_approval`은 120 = 2분, 문 앞에서 바로 승인하는 경우만),
  거절 코드 `PROPOSAL_EXPIRED`. 스키마는 30~86400초 정수. `validate-contracts.py`가 "현관문 열기 120초, 나머지 대기
  작업 600초, 승인 대기 규칙에만 붙음, 거절 코드 있음"을 고정 검사한다
- core: 1초마다 `proposed` 작업을 보고, 작업이 처음 들어올 때 고른 정책 규칙의 시간이 지나면 `rejected`
  (`rejection_code` `PROPOSAL_EXPIRED`, 메시지 "not approved within N s")로 닫고 task update를 보낸다(로그
  `PROPOSAL_EXPIRED`). 시간은 작업 생성 시각(벽시계)과, 이번 core 실행에서 대기를 처음 본 뒤 흐른 시간(가동 시간) 중
  먼저 차는 쪽으로 잰다 — Pi4는 RTC가 없어 시계가 뒤로 가도 오래된 제안이 살아나지 않게
- 정리 전에 도착한 승인(화면에 아직 남아 있던 옛 제안): core가 그 자리에서 만료로 닫고 아무것도 실행하지 않는다
  (`user_feedback.error_text` `proposal_expired`)
- 정책 파일이 version 2(시간 없음)여도 core는 멈추지 않고 600초를 쓴다. 설치 스크립트는 패키지의 v3로 바꾼다
- heartbeat 버전 문자열 `pi4-core-0.6.4`, control 0.6.4
- 7인치 UI·휴대폰 앱: `PROPOSAL_EXPIRED`를 "승인 시간 지남 / 승인을 기다리는 시간이 지나 실행하지 않고 닫았어요"로 표시,
  휴대폰에서 이미 닫힌 요청을 승인하면 "이미 처리됐거나 승인 시간이 지난 요청입니다"

0.6.3과 같이 `mosquitto.conf`·`mosquitto.acl`은 없을 때만 복사된다(실기 Pi4의 secure-broker 설정은 그대로).

## 시험 (PC, 시뮬레이션 — Pi4 실기 아님)
| 시험 | 결과 |
|---|---|
| `contracts/validate-contracts.py` + 변형 시험 | PASS, 44개(새 4개: 현관문이 기본 시간으로 기다림·기본 시간 없음·자동 규칙에 시간·거절 코드 없음은 실패) |
| `tests/run-pc-docker-test.sh 20` (x86) | PC_DOCKER_TEST_PASS. 새 단계 `tests/proposal-expiry-check.py`(정책 사본 기본 45초·현관문 30초): 현관문 요청 30.6초에 만료·그때 LLM 제안은 대기, LLM 제안 45.6초에 만료, 만료 뒤 옛 리비전·토큰으로 승인 → 바뀌는 것 없음·명령 0, task update에 `PROPOSAL_EXPIRED` |
| IPK를 arm64 Ubuntu에 풀어 core + selftest (QEMU) | SELFTEST_PASS, CORE_STARTED |

IPK `dist-ipk/homemind-core_0.6.4_aarch64.ipk` 991,898 bytes, SHA-256 `d8f0ab25…`. 0.6.3 IPK는 되돌리기용으로 둔다.
설치는 사용자 승인 후 `deploy/NODE-RPI4-01/install.sh`(운영 DB 온라인 백업 → 설치 → `-opkg` 설정 채택 → 재시작).
설치 뒤 실기 Pi4 DB에 남아 있던 대기 작업은 첫 정리 때 만료로 닫힌다(삭제 아님, 상태만 바뀜).
