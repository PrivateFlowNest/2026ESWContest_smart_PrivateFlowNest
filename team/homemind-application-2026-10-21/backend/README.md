# HomeMind Mobile API 0.4 (beta, core 경유 + QR 페어링 토큰) — Python 기준 구현

휴대폰 앱(Flutter)과 RPI4-01 core 사이의 HTTP 경계. system-architecture 3-5절 결정(2026-10-21)대로
**장치 명령은 core를 거친다**: `POST /api/v1/devices/{device_id}/commands` → `pfn/v1/core/task/request`
(intent `device_control`, `parameters.origin: "ui"`) → core가 registry·task-policy로 검사 → ESP 명령 → ACK →
`pfn/v1/core/task/update`. 장치 상태는 core가 작업 `completed`(ACK 이후)를 알린 뒤에만 바뀐다.
옛 `homemind/v1/commands` 직접 발행과 낙관적 상태 변경은 없앴다.

**기기 실행 형태 (2026-10-22 사용자 결정)**: RPI4-01에서 core 옆에 **C++ 네이티브 서비스**
`native/services/homemind-mobile-api`로 돈다(기기에 Python 없음). 이 Python 코드는 PC 기준 구현(reference)이며,
C++와 같은 요청에 같은 응답을 내는지 `native/tests/test_mobile_api_parity.py`로 대조한다. 두 구현은 페어링 알고리즘과
파일 형식도 같다.

| API | 인증 | 설명 |
|---|---|---|
| `GET /api/v1/health` | 없음 | 상태·버전(`0.4.0-beta`)·MQTT 연결 여부 |
| `POST /api/v1/pairing/claim` `{"code","nonce","device_name"}` | 없음(일회용 코드) | 201 `{"token","token_id","token_type":"Bearer","device_id","device_name"}`. 틀림·만료·재사용 401, 실패가 많으면 429 + `Retry-After` |
| `GET /api/v1/hardware-map`, `/devices`, `/dashboard` | Bearer | `contracts/pfn.v1/device-registry.json`(v3)에서 만든 장치 목록. 배선 안 된 장치는 명령 없음 |
| `POST /api/v1/devices/{id}/commands` `{"command":"OPEN\|CLOSE\|ON\|OFF"}` | Bearer | 202 + `task_id`. 서보는 registry `positions`, 그 외 `set_switch`. 현관문 열기는 7인치 UI 승인 대기(`proposed`) |
| `GET /api/v1/tasks/{task_id}` | Bearer | `requested → ready/running → completed`, 또는 `rejected/failed` + `failure_code` |

`/api/` 아래는 health·claim을 빼고 모두 `Authorization: Bearer <token>`이 필요하다(라우팅 전에 검사: 토큰 없으면
없는 경로도 401). 401에는 `WWW-Authenticate: Bearer`.

## QR 페어링과 토큰 (`app/auth.py`, C++ `src/auth.hpp`와 동일)
1. 기기에서 사람이 QR을 만든다: `python -m app.cli --print-pairing-qr` (C++: `homemind-mobile-api --config … --print-pairing-qr`).
   출력: 일회용 코드(`ABCD-EFGH`, 기본 10분)와 앱이 읽는 QR JSON
   `{"version":1,"device_id","device_name","base_url","pairing_code","nonce","expires_at"}` (`lib/models/pairing_models.dart` 그대로).
2. 앱이 QR을 스캔 → 사용자가 기기 화면의 코드와 같은지 보고 승인 → `POST /api/v1/pairing/claim`
   (`code`, `nonce`, `device_name`=휴대폰 이름). **claim 본문에 `nonce`를 추가했다**(코드가 nonce에 묶여 있어서).
3. 서버는 nonce(`v1.<만료 unix 초>.<32 hex>`)의 만료·최대 수명(3600 s)·`HMAC-SHA256(페어링 비밀, "homemind-pairing-v1\n"+nonce)`
   앞 5바이트의 base32·재사용 여부를 검사하고 토큰(`hm1_` + 64 hex)을 한 번 준다.
4. 서버는 토큰의 SHA-256만 `<data_dir>/tokens.json`(0600)에 저장한다. 앱은 토큰을 Android Keystore 기반
   `flutter_secure_storage`에 두고 모든 요청에 Bearer로 보낸다. 401이면 저장된 토큰을 지우고 페어링 화면으로 돌아간다.
5. 폐기: `python -m app.cli --list-tokens`, `--revoke-token ID`, `--revoke-all-tokens`. 실행 중인 서버가 다음 요청에서 반영.

페어링 비밀은 `<data_dir>/pairing-secret`(또는 `HOMEMIND_PAIRING_SECRET_FILE`)에만 있고, 없으면 서버/CLI가 0600으로 만든다.
실패한 claim은 주소당 5회·전체 20회/300 s로 제한한다. 코드·토큰·비밀은 로그에 남기지 않는다.

## 설정 (환경 변수, `.env.example`)
MQTT 계정은 `homemind-mobile-api`(core 패키지 `mosquitto.acl`), 비밀번호는 `HOMEMIND_MQTT_PASSWORD_FILE` 경로로만
받는다(`HOMEMIND_MQTT_PASSWORD`는 거부). `HOMEMIND_SOURCE_NODE_ID`는 task-policy의 origin `ui` 허용 노드
(NODE-RPI4-01 또는 NODE-RPI5-01)여야 한다. 페어링: `HOMEMIND_DATA_DIR`(기본 `~/.homemind/mobile-api`),
`HOMEMIND_PAIRING_SECRET_FILE`, `HOMEMIND_PUBLIC_BASE_URL`(QR의 `base_url`), `HOMEMIND_DEVICE_ID`, `HOMEMIND_DEVICE_NAME`,
`HOMEMIND_PAIRING_CODE_TTL_S`, `HOMEMIND_PAIRING_MAX_FAILURES_PER_CLIENT`, `HOMEMIND_PAIRING_MAX_FAILURES_GLOBAL`,
`HOMEMIND_PAIRING_FAILURE_WINDOW_S`. 비밀값 자체(`HOMEMIND_PAIRING_SECRET`)는 거부한다.
평문 HTTP는 개발 LAN 전용이다(TLS는 범위 밖).

## 시험 (PC)
```powershell
cd backend
python -m venv .venv
.\.venv\Scripts\python.exe -m pip install -r requirements-dev.txt
.\.venv\Scripts\python.exe -m pytest -q
# PC 데모 스택(demo/pc, core + 가짜 ESP)이 떠 있으면 E2E도:
$env:HOMEMIND_E2E="1"; .\.venv\Scripts\python.exe -m pytest -q
```
2026-10-22 (0.4): `tests/test_api.py` + `tests/test_auth.py` 19개(E2E 포함) 통과 — 기존 장치 경로 시험은 토큰을 붙여 그대로,
페어링·토큰 시험(코드 벡터가 C++와 같음, 일회용·만료·틀림·429, CLI 폐기 즉시 401, 손상된 tokens.json은 모두 거부,
파일 0600 — Windows에서는 모드 확인 생략). **PC 시뮬레이션이며 실제 Pi4·ESP 시험 아님.**
