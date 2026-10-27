# homemind-mobile-api (NODE-RPI4-01, C++)

휴대폰 앱(Flutter, `team/homemind-application-2026-10-21`)의 HTTP API. 2026-10-22 사용자 결정대로 **RPI4-01에서
homemind-core 옆에 C++ 네이티브 서비스로** 돈다(LG AI Native OS에는 Python이 없음). Python Mobile API
(`team/homemind-application-2026-10-21/backend`, 0.4)가 기준 구현이고, 같은 요청에 같은 응답을 내는지
`native/tests/test_mobile_api_parity.py`로 대조한다.

- core와는 **pfn.v1 MQTT로만** 말한다: 명령 → `pfn/v1/core/task/request`(intent `device_control`,
  `parameters.origin: "ui"`, `source_node_id` NODE-RPI4-01) → core 검사 → ESP → ACK → `pfn/v1/core/task/update`.
  장치 상태는 core가 `completed`를 알린 뒤에만 바뀐다. 장치 토픽(`pfn/v1/device/...`, `homemind/v1/...`)은 쓰지 않는다.
- MQTT 계정 `homemind-mobile-api`(core 패키지 `mosquitto.acl`), 비밀번호는 `broker.password_file` 경로로만.
- **인증 없이 열지 않는다**: `/api/v1/health`, `/api/v1/pairing/claim`을 뺀 모든 `/api/` 요청에 `Authorization: Bearer <token>`.

## API (Python 0.4와 같은 모양)

| API | 인증 | 설명 |
|---|---|---|
| `GET /api/v1/health` | 없음 | 상태·버전·MQTT 연결 여부(집 데이터 없음) |
| `POST /api/v1/pairing/claim` `{"code","nonce","device_name"}` | 없음(일회용 코드) | 201 `{"token","token_id","token_type":"Bearer","device_id","device_name"}`. 실패 401, 실패가 많으면 429 + `Retry-After` |
| `GET /api/v1/dashboard`, `/hardware-map`, `/devices` | 토큰 | `device-registry.json`(RPI4-01에서는 core가 쓰는 `/data/share/homemind/config/device-registry.json`)으로 만든 목록. 배선 안 된 장치는 명령 없음 |
| `POST /api/v1/devices/{id}/commands` `{"command":"OPEN\|CLOSE\|ON\|OFF"}` | 토큰 | 202 + `task_id`. 서보는 registry `positions`, 나머지 `set_switch`. 현관문 열기는 7인치 UI 승인 대기(`caution` → `proposed`) |
| `GET /api/v1/tasks/{task_id}` | 토큰 | `requested → ready/running → completed`, 또는 `rejected/failed` + `failure_code`. 휴대폰이 승인한 다른 작업은 core가 마지막으로 알린 상태만 |
| `GET /api/v1/approvals` (0.5) | 토큰 | 승인 대기 작업 `{"approvals":[{task_id,title,intent,target_device,origin,phone_can_approve,note,waiting_since_ms}]}`. action_token은 서버에만 |
| `POST /api/v1/tasks/{task_id}/approve`, `/reject` (0.5) | 토큰 | 202 `{"status":"sent"}` → `pfn/v1/mobile/task/action`. 대기 아님 404, 현관문 열기 승인 403, MQTT 끊김 503 |
| `GET /api/v1/events` (0.5) | 토큰 | server-sent events: `task`(작업 상태), `approvals`(대기 수). 15초마다 `: keep-alive`, `?after=<seq>`로 이어 받기 |

오류는 FastAPI와 같다: `{"detail": "..."}`, 본문 검사 실패는 422 + pydantic 형식 목록, 없는 경로 404, 메서드 405 + `Allow`.
토큰 없음/틀림 401 + `WWW-Authenticate: Bearer`. CORS는 없다(Android 앱은 필요 없음). `/`는 링크만, `/docs`는 Python만.

## 휴대폰 승인·실시간 알림 (0.5, core 0.6.3 필요, 2026-10-22 결정)
- `ApprovalBook`(`src/approvals.cpp`)이 core가 알리는 모든 작업(`pfn/v1/core/task/update`, 상태 스냅샷의 `tasks`)을 보고
  `proposed` + action_token인 작업을 승인 대기로 둔다. 리비전이 낮은 늦은 업데이트는 무시, 스냅샷에 없는 대기는 지운다.
- 휴대폰 승인·거절은 7인치 UI와 같은 문서(`ui-action.schema.json`)를 **별도 토픽 `pfn/v1/mobile/task/action`**으로 보낸다.
  브로커 ACL상 이 계정만 쓸 수 있고 `pfn/v1/ui/#`에는 쓸 수 없어서, core가 휴대폰에서 온 것을 안다.
- **현관문 열기(SERVO-DOOR-01)는 휴대폰으로 승인할 수 없다**(거절은 가능). 이 서비스가 403으로 막고, core도
  `task-policy.json` `phone_may_approve: false` 규칙으로 휴대폰 토픽의 approve·resume을 거부한다(`PHONE_APPROVAL_REFUSED`).
- 실시간 알림은 SSE(`EventHub`, 최근 200개): 앱이 켜져 있을 때만 받는다. 앱이 꺼져 있을 때의 OS 푸시(FCM)는 범위 밖
  (외부 서비스·Google 계정이 필요). 서비스 종료 시 열린 스트림을 먼저 닫는다.
- Python 기준 구현(0.4)에는 이 세 API가 없다. C++ 단위 시험과 PC 스택 실경로 시험으로 확인한다.

## 페어링·토큰

| 항목 | 내용 |
|---|---|
| 페어링 비밀 | `pairing_secret_file`(기본 `<data_dir>/pairing-secret`): 64자 hex(32바이트 난수) 한 줄, 0600. 없으면 서비스/CLI가 만든다. 기기 밖으로 나가지 않는다 |
| QR | 기기에서 사람이 `homemind-mobile-api --config … --print-pairing-qr` 실행 → 일회용 코드 + QR JSON 출력(터미널에만). 앱의 `PairingPayload`(version 1) 필드 그대로: `version, device_id, device_name, base_url, pairing_code, nonce, expires_at` |
| nonce | `v1.<만료 unix 초>.<32 hex 난수>` |
| 코드 | `base32(HMAC-SHA256(비밀, "homemind-pairing-v1\n" + nonce)[:5])` → 8자, `ABCD-EFGH`로 표시. 기본 10분(`pairing_code_ttl_s`, 30~3600 s). 한 번만(쓴 nonce는 만료까지 `<data_dir>/pairing-used.json`) |
| claim | 앱이 QR 승인 후 `{code, nonce, device_name}` 전송. 서버는 만료·최대 수명·HMAC·재사용을 검사하고 토큰 발급 |
| 토큰 | `hm1_` + 64 hex(32바이트 난수). 평문은 claim 응답에 한 번만. 서버는 `SHA-256(token)`만 `<data_dir>/tokens.json`(0600)에 id(8 hex)·휴대폰 이름·생성 시각과 저장. 폐기 전까지 유효(자동 만료 없음) |
| 실패 제한 | 실패한 claim: 주소당 5회, 전체 20회 / 300 s(설정 가능) → 429 + `Retry-After` |
| 폐기 | `--list-tokens`, `--revoke-token ID`, `--revoke-all-tokens`. 서비스는 `tokens.json` 변경(mtime·크기·inode)을 다음 요청에서 읽어 바로 401 |
| 로그 | 코드·토큰·비밀은 남기지 않는다(토큰 id, 휴대폰 이름, 주소, 실패 이유만) |

같은 알고리즘·파일 형식을 Python 기준 구현(`backend/app/auth.py`, `python -m app.cli`)도 쓴다. C++ CLI가 만든 QR을
Python 서버가 받고 그 반대도 된다(대조 시험에서 확인).

### 7인치 UI에 QR 띄우기 (2026-10-22, 계약 `mobile-pairing-request/offer`)
UI(NODE-RPI5-01)가 `pfn/v1/mobile/pairing/request`를 보내면 `PairingRelay`(`src/pairing_relay.cpp`)가 터미널 QR과 같은
`PairingService` 제안(일회용 코드 + nonce, `pairing_code_ttl_s`)을 `pfn/v1/mobile/pairing/offer`로 돌려준다. 같은 request_id를
되받는 요청, 만료·형식 오류·다른 대상은 무시, 2초 안의 연속 요청과 `public_base_url` 없음은 `status: unavailable`(코드 없음).
코드는 로그에 남기지 않는다(요청 id 앞 8자와 만료 시각만). core 0.6.2 ACL: NODE-RPI5-01만 offer를 읽고 이 계정만 쓴다.

### 7인치 UI의 연결된 휴대폰 목록·연결 해제 (2026-10-25, 계약 `mobile-phones-request/state`, Mobile API 0.2.1)
UI가 `pfn/v1/mobile/phones/request`(`action` list | revoke + `phone_id`)를 보내면 `PhonesRelay`(`src/phones_relay.cpp`)가
`pfn/v1/mobile/phones/state`로 토큰이 유효한 휴대폰 목록을 돌려준다: `phone_id`(토큰 id, 토큰 아님), 페어링 때 보낸 이름, 페어링
시각, 이번 실행에서 마지막으로 받아들인 요청 시각(`last_seen_at_ms`, 없으면 null). revoke는 `tokens.json`에서 바로 지운다: 그
휴대폰의 다음 요청은 401(앱은 QR 화면으로), 열려 있던 SSE는 다음 keep-alive(15초) 안에 끊긴다. 휴대폰이 새로 페어링되면
요청 없이 목록을 보낸다(`request_id` null, `action` paired). 형식 오류·만료·재전송·다른 대상은 무시. ACL: NODE-RPI5-01만 목록을
읽고 이 계정만 쓴다(`install-mobile-api.sh` 1b단계가 기기 ACL이 core 0.6.3/0.6.4 원본일 때만 새 ACL로 바꾸고 HUP).

## 설정 (`/data/share/homemind/config/mobile-api.json`, 템플릿 `packaging/mobile-api.json`)
`hm::RuntimeConfig` 키(`base_dir`, `broker{host,port,username,password_file,keepalive}`, `contracts_dir`) +
`device_registry`, `mqtt_enabled`, `mqtt_client_id`, `source_node_id`(NODE-RPI4-01|NODE-RPI5-01), `listen_host`,
`listen_port`, `http_threads`, `data_dir`(필수), `pairing_secret_file`, `public_base_url`(QR의 주소: 휴대폰이 닿는
RPI4-01 주소, 공유기 고정 IP 확정 후 [미정]), `device_id`, `device_name`, `pairing_code_ttl_s`,
`pairing_max_failures_per_client`, `pairing_max_failures_global`, `pairing_failure_window_s`.
비밀값 키(`broker.password`, `pairing_secret`, `tls_key`)는 거부한다. HTTPS는 `tls_enabled`(+ `tls_cert_file`, `tls_key_file`),
기기 템플릿은 켜져 있고 PC 개발 기본값은 꺼짐(http).

## HTTPS (0.5, 2026-10-22 결정)
- 공인 인증서·도메인 없이 쓰려고 **자체 서명 인증서 + QR 지문 고정**: 서비스가 처음 시작할 때(또는 `--print-pairing-qr`)
  EC P-256 키와 10년 인증서(CN = device_id, SAN = `public_base_url`의 IP)를 `data_dir`에 만든다(키 0600, 기기 밖으로 안 나감).
- 인증서 DER의 SHA-256이 QR의 `tls_sha256`(https 주소일 때만). 앱과 7인치 UI 경유 QR 모두 같은 값. 앱은 그 인증서만 믿는다
  (`SecurityContext(withTrustedRoots: false)` + 지문 비교). 다른 인증서·지문 없는 https는 연결 거부
- 인증서를 다시 만들면(파일 삭제) 지문이 바뀌므로 휴대폰은 새 QR로 다시 연결해야 한다. 키와 인증서가 짝이 안 맞으면 시작 거부
- 빌드: OpenSSL 3 헤더(`libssl-dev`, `native/docker/dev.Dockerfile`)가 있어야 HTTPS 포함(`--version`에 `+tls`). 패키지 빌드는
  `+tls`가 아니면 실패한다. 기기의 libssl.so.3은 core(libmosquitto)가 이미 쓰는 것을 같이 쓴다(동봉 안 함)
- PC 시험: `bash deploy/PHONE-APP/pc-mobile-api.sh <PC IP> --https` → TLS 1.3, 서버 인증서 지문 = QR, 같은 포트 평문 http 거부

## 사용 (기기, 사람이 실행)
```sh
# 휴대폰 연결: QR 출력(코드는 10분, 한 번만). QR은 로컬에서 그린다(예: qrencode -t ANSIUTF8 '<JSON>').
# 온라인 QR 생성기에 붙여 넣지 않는다(살아 있는 일회용 코드가 들어 있다).
/data/share/homemind/bin/homemind-mobile-api --config /data/share/homemind/config/mobile-api.json --print-pairing-qr
# 연결된 휴대폰 보기 / 폐기
/data/share/homemind/bin/homemind-mobile-api --config /data/share/homemind/config/mobile-api.json --list-tokens
/data/share/homemind/bin/homemind-mobile-api --config /data/share/homemind/config/mobile-api.json --revoke-token <id>
```
root로 실행하면 새로 쓰는 파일(`tokens.json`, 비밀)의 소유자를 `data_dir` 소유자(서비스 사용자 `network`)로 맞춘다.

## PC 빌드·시험 (시뮬레이션)
```bash
# x86 단위 시험 (Docker 볼륨 hm-native-build-mobile)
docker run --rm -v "$PWD:/w" -v hm-native-build-mobile:/build homemind-native-dev:24.04 bash -c \
  "cmake -S /w/native -B /build -G Ninja && cmake --build /build --target homemind_mobile_api_tests homemind-mobile-api && /build/services/homemind-mobile-api/homemind_mobile_api_tests"
# Python ↔ C++ 대조 + PC core E2E (demo/pc compose 실행 중일 때)
services/runtime/.venv/Scripts/python -m pytest native/tests/test_mobile_api_parity.py -s
```
### 결과 (2026-10-22, PC 시뮬레이션 — Pi 실기 아님)
- x86 gtest 18/18(crypto 벡터, 페어링·토큰·429·폐기, API·422·405, 설정), 전체 native ctest 14/14, `-Werror` 경고 0
- `test_mobile_api_parity.py` 2회: 53/53 단계 동일(페어링 교차 claim, 인증 게이트, 읽기·검사, 명령 6개는 PC core +
  가짜 ESP로 `completed`, 현관문 `proposed`, CLI 폐기, 실패 제한 지점 같음), 발행한 task request 전부 스키마 통과, 두 로그에 토큰·코드·비밀 없음
- aarch64(QEMU, `-mcpu=cortex-a72`, Release): gtest 18/18, `ldd` = libmosquitto.so.1 + 기기 기본(libstdc++, libgcc_s, libc, libm,
  libssl/libcrypto 3 — libmosquitto가 끌어옴), GLIBC ≤ 2.38, GLIBCXX ≤ 3.4.32

aarch64(RPI4 = Cortex-A72, ARMv8.0)는 `-mcpu=cortex-a72`로 빌드한다. RPI5용 `build-rpi5.sh`의 `-mcpu=cortex-a76`
(ARMv8.2: LSE 원자 명령 등)로 만든 바이너리는 Pi 4에서 SIGILL로 죽을 수 있으니 RPI4 패키지에 쓰지 않는다.
**PC·QEMU 결과는 Pi 실기 결과가 아니다.**
- 0.5(휴대폰 승인·SSE): gtest 26/26(새 4: 승인 목록·403·202·404·503, 늦은 업데이트·스냅샷 정리, 계약 필드, EventHub),
  arm64 Cortex-A72 빌드 ctest 통과(QEMU). PC 스택 실경로 23/23(core 0.6.3 + 가짜 ESP), 앱 클라이언트 실연결 통과.
  대조 시험은 health의 `version`만 빼고 같음(C++ 0.5 = Python 0.4 + 승인·이벤트)
