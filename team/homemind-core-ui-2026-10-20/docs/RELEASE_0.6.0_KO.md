# homemind-core 0.6.0 (2026-10-21, P3)

**실제 Pi4에 설치하지 않았다.** PC Docker(x86) 시뮬레이션과 arm64 빌드까지만 했다. 설치는 사용자 확인 후.

## 바뀐 것

| 항목 | 0.5.2 | 0.6.0 |
|---|---|---|
| 계약 위치 | 팀 폴더 `contracts/` | 저장소 루트 `contracts/pfn.v1/` (빌드 시 `--build-context contracts=...`) |
| 구독 | `pfn/v1/#` | `protocol.json core_subscriptions` 8개. `pfn/v1/ai/llm/*` 제외 |
| LLM 토픽 | (없었음) 구독하면 원문이 `mqtt_events`에 저장될 구조 | 구독 안 함 + 코드에서 저장 전에 버림 + ACL로 core 계정 접근 불가 |
| registry | 읽지 않음 | `--device-registry` 필수. 시작 시 시나리오·상황 규칙이 배선된 장치·capability·각도·색에 맞는지 검사, 안 맞으면 시작 거부 |
| `device_control` | 없음 | 단일 장치 명령. 제어기 노드는 registry에서 정함. 배선·capability·파라미터·LED 색·서보 등록 각도 검사 |
| 승인 | 모든 Task가 UI 승인 대기 | `task-policy.json`: 음성 파서·UI(휴대폰) 직접 명령과 조용 모드는 바로 실행. LLM 제안, 현관문 열기, 기존 시나리오 3개는 UI 승인 |
| 거절 | 형식 오류만 버림 | 정책·registry 위반은 `rejected` Task + `failure.code`(DEVICE_NOT_WIRED 등)로 UI에 알림 |
| 상황 규칙 | perception 저장만 | `context-rules.json`: ACTIVITY_CONTEXT quiet ACTIVATED → `quiet_mode_on`, DEACTIVATED → `quiet_mode_off` |
| perception | 형식만 검사 | 타입 3종, registry zone, 발신 노드 = 인식 노드(RPI5-02), 장치-노드 연결 검사 |
| LED | `set_color` 색만 | `pattern`(steady/blink) 검사 |
| 비밀번호 | `--password <값>` (프로세스 목록에 노출) | `--password` 거부, `--password-file <경로>`만 |
| DB | schema 4 | schema 5: `tasks.rejection_code`, `tasks.rejection_message` 추가(ALTER, 기존 데이터 유지) |
| 브로커 ACL | 없음 | `package/mosquitto.acl` 템플릿(계정 = 노드 ID, ESP8266 자리 포함). 패키지는 설정 파일로 복사만 하고 활성화하지 않음 |

## 설치 전 확인 (사용자)

1. 운영 DB 백업: `/data/share/homemind/db/homemind.db` (마이그레이션은 컬럼 추가만 하지만 백업 먼저)
2. `/data/share/homemind/config/`의 `scenario-rules.json`·`device-registry.json`을 손으로 고친 적이 있으면 opkg가 conffile을 유지할 수 있다. 0.6.0 core는 registry v3과 scenario v2가 함께 있어야 시작한다(옛 scenario v1은 배선 안 된 LED-GUIDE-01을 써서 시작 거부)
3. 새 config 2개(`task-policy.json`, `context-rules.json`)가 설치되는지 확인
4. systemd unit은 postinst가 `/etc/systemd/system/homemind-core.service`로 복사(새 인자 포함)
5. ACL을 켤 때: `mosquitto.passwd` 사용자 이름을 노드 ID(`NODE-RPI4-01`, `NODE-RPI5-01`, `NODE-RPI5-02`, `CTRL-ESP32-ACT-01`, `CTRL-ESP8266-LED-01`, `homemind-mobile-api`)로 만든 뒤 1884 리스너에 `acl_file` 줄을 넣는다. 지금 Pi4의 계정 이름은 저장소에 없으므로 확인 필요. 1883·9001 개발용 익명 리스너에는 ACL이 없다(직결 케이블 전용)
6. 되돌리기: `dist-ipk/homemind-core_0.5.2_aarch64.ipk`(SHA-256 `13aeca1b…`). DB schema 5는 0.5.2 코드상 열릴 것으로 보인다(추가 컬럼은 쓰지 않음, schema_version은 4로 되돌려 씀). **시험하지 않았다**

## 시험 (PC, 시뮬레이션)

- `tests/run-pc-docker-test.sh`: x86 빌드, registry 불일치 시작 거부, `--password` 거부, 레거시 DB 마이그레이션 + selftest(새 정책 경로 14개 포함), soak 단축판
- `tests/run-acl-test.sh`: 실제 Mosquitto + `package/mosquitto.acl`로 13개 허용·차단 확인
- 결과는 `docs/handoff/PROGRESS.md` P3 항목
