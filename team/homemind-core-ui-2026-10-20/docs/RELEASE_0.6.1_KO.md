# homemind-core 0.6.1 (2026-10-22)

0.6.0과 core 동작은 같다. 바뀐 것은 MQTT ACL 템플릿과 버전 표기뿐이다.

- 계약 `pfn/v1/ai/voice/listen`(7인치 UI 누름 통화 → RPI5-02) 추가에 맞춰 `package/mosquitto.acl`에
  NODE-RPI5-01 `write`, NODE-RPI5-02 `read` 추가. core 계정은 이 토픽에 접근하지 못한다(`pfn/v1/ui/#` 밖)
- heartbeat 버전 문자열 `pi4-core-0.6.1`, control·serviceinfo 0.6.1

## 시험 (PC, 시뮬레이션 — Pi4 실기 아님)
| 시험 | 결과 |
|---|---|
| `tests/run-pc-docker-test.sh 60` (x86) | PC_DOCKER_TEST_PASS (빌드, registry 검사, `--password` 거부, selftest, v5 마이그레이션, soak) |
| `tests/run-acl-test.sh` (실제 Mosquitto) | ACL_TEST_PASS, 16개(새 3개: UI → RPI5-02 전달, core 차단, 휴대폰 차단) |
| IPK를 arm64 Ubuntu에 풀어 core + selftest (QEMU) | SELFTEST_PASS, 전사 저장 0건 |

IPK `dist-ipk/homemind-core_0.6.1_aarch64.ipk` 971,120 bytes, SHA-256 `5391ab89…`. 0.6.0 IPK는 그대로 둔다.
설치 방법·확인 사항은 `RELEASE_0.6.0_KO.md`와 같다(운영 DB 백업, conffile, ACL 활성화는 계정 확인 후). 설치는 사용자 승인 후
`deploy/NODE-RPI4-01/install.sh`.
