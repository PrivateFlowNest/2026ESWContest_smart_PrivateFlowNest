# HomeMind 팀 인수인계서

> **2026-10-21 변경**: `contracts/`는 저장소 루트 `contracts/pfn.v1/`로 옮겼다(정본 단일화). 이 문서의 `contracts/...` 경로는 그 위치로 읽는다. 검증: `python tests/validate-contracts.py`(루트 검증기 래퍼) 또는 `python contracts/validate-contracts.py`. Docker 빌드는 `--build-context contracts=../../contracts/pfn.v1`(scripts/build*.ps1에 반영).

작성일: 2026-10-20  
전달물: `HomeMind_Full_Handoff_2026-10-20.zip`  
대상: Pi4 백엔드, Pi5/Flutter UI, 비전, ESP32·ESP8266·Octopus 담당자

## 1. 이 문서부터 읽기

HomeMind는 다음 역할로 나뉜다.

```text
Pi5 카메라/비전 ─┐
ESP32 센서 ──────┼─ MQTT ─ Pi4 백엔드 + SQLite ─ MQTT ─ Flutter UI
ESP32 액추에이터 ┘                 │
                             Task 생성·명령·기록
```

- **Pi4**: MQTT 메시지 수집, 센서값 검증, Task 관리, 장치 명령, ACK 확인, SQLite 저장
- **Pi5**: 최종 Flutter UI 실행과 카메라 기반 사람/인형 인식
- **ESP32 계열**: 센서값·heartbeat 발행, 명령 수신, 실제 장치 작동, ACK 반환
- **Flutter UI**: Pi4 상태 표시, Task 승인·거절, 개인정보 설정, 피드백 전달
- **MQTT**: 장치 사이의 메시지 통로
- **SQLite**: Pi4 내부의 영구 기록장

UI가 ESP32를 직접 제어하지 않는다. 명령과 현재 상태의 기준은 항상 Pi4다.

## 2. 현재까지 완료된 범위

> 이 ZIP에는 현재 작업 폴더에서 작성한 코드가 전부 들어 있다. 다만 ESP32 실장비 펌웨어와
> Pi5 비전 인식 코드는 아직 작성되지 않았으므로 최종 프로젝트 전체 코드가 완성된 상태는 아니다.
> 정확한 파일 수와 미구현 범위는 `docs/CODE_INVENTORY_KO.md`를 참고한다.

현재 소스·설정 파일은 119개, 약 14,461줄이다. ZIP이 작아 보이는 이유는 200MB 이상의
Flutter/CMake 빌드 캐시를 제외했고 텍스트 소스가 압축되기 때문이다.

### Pi4 백엔드

- `pfn/v1/#` MQTT 메시지 수신 및 원문 보존
- JSON 스키마·공식 장치 ID·센서 자료형·범위 검증
- commands, ACK, telemetry, heartbeat, Task, 피드백, 안전 상태의 SQLite 저장
- SQLite WAL, foreign key, busy timeout, transaction 적용
- 동일 `event_id` 중복 처리 방지
- command ACK 제한 3초, 최대 3회, 전체 유효시간 10초
- 같은 command를 재시도할 때 동일 `command_id`와 `trace_id` 유지
- ACK 시간 초과시 Task `failed`와 `ACK_TIMEOUT` 기록
- heartbeat 5초, 마지막 메시지 15초 후 장치 `offline` 판정
- 저장공간 사용률 70% 경고
- 자동 데이터 삭제 비활성화
- 인터넷 없이 Pi4 시각이 틀렸을 때 잘못된 날짜를 정상 시각처럼 취급하지 않는 보호
- 귀가, 취침 준비, 야간 침대 이탈 세 시나리오 구현
- P0 안전 경고, 개인정보 상태, 사용자 피드백 처리
- systemd 자동 시작용 배포 파일과 Atlas IPK 제작 소스

### Flutter UI

- 목표 해상도 1024×600
- 하단 메뉴: 홈, Task, 기기, 개인정보, 더보기
- Context, 신뢰도, 판단 근거, 최근 Task 표시
- Task 목록·필터·상세 진행 단계
- 실제 `intent`, 대상 장치, 신뢰도, revision, ACK 결과 표시
- 승인·거절·취소 전송
- 장치 온라인·오프라인, 센서값, FND 값, 마지막 수신 시각 표시
- 개인정보 설정의 Pi4 왕복 연결
- Context/동작 피드백의 Pi4 SQLite 저장
- P0 경고 전체 화면과 사용자 ACK
- 오프라인 안전 모드와 자동 MQTT 재연결
- 기록·시스템 상태·오류 화면
- 노트북 웹 미리보기와 Pi5 Atlas용 프로젝트 골격

### 실제 확인한 결과

마지막 Pi4 실장비 통합 확인일은 2026-09-04이다.

- Pi4 Core, MQTT, SSH socket 활성 상태 확인
- SQLite `integrity_check = ok`
- `prepare_home`, `prepare_sleep`, `night_bed_exit` 세 시나리오 성공
- 가짜 `CTRL-ESP32-ACT-01`, `CTRL-ESP8266-LED-01`의 실제 MQTT ACK 확인
- 개인정보 `일반 → 카메라 차단 → 일반` 왕복 확인
- 피드백 SQLite 저장 확인
- P0 표시, 사용자 ACK, 활성 경고 0건 복귀 확인
- 가짜 장치 중단 17초 후 offline 확인
- MQTT broker 5초 중단 후 Flutter UI 자동 재연결 확인
- Flutter 정적 분석 오류 0개
- Flutter 자동 테스트 16개 통과, 실장비 전용 1개 보류
- Flutter 웹 release 빌드 성공

상세 시험 결과는 `reports/ui-pi4-integration-20260904.md`를 참고한다.

## 3. 압축파일 폴더 설명

```text
HomeMind_Full_Handoff_2026-10-20/
├─ README_TEAM_HANDOFF_KO.md      ← 지금 읽는 문서
├─ README.md                      ← Pi4 백엔드 개요
├─ .pi4_known_hosts               ← 현재 Pi4 SSH host key
├─ CMakeLists.txt                 ← C++ 빌드 정의
├─ Dockerfile                     ← ARM64/IPK 빌드 환경
├─ src/                           ← Pi4 C++ 백엔드, simulator, selftest
├─ sql/                           ← SQLite 초기 스키마
├─ contracts/                     ← MQTT 토픽·JSON·공식 장치 ID 계약
├─ deploy/                        ← systemd와 Mosquitto 배포 설정
├─ package/                       ← Atlas IPK 제작 소스
├─ dist/                          ← 빌드된 ARM64 실행 파일과 라이브러리
├─ dist-ipk/                      ← 설치 가능한 IPK 3개와 SHA256SUMS
├─ third_party-packages/          ← 오프라인 재빌드용 ARM64 패키지 원본
├─ scripts/                       ← 빌드·설치·상태·시험 자동화
├─ tests/                         ← 계약·DB·마이그레이션 자동검사
├─ homemind_flutter_test/         ← Flutter UI 소스
│  ├─ lib/
│  ├─ test/
│  ├─ tool/
│  ├─ atlas/
│  ├─ linux/
│  ├─ windows/
│  └─ web/
├─ docs/                          ← 역할별 계약과 운영 안내
└─ reports/                       ← 통합시험 결과
```

다음 항목만 의도적으로 압축파일에서 제외했다.

- 임시 시험 SQLite DB
- Flutter `build/`, `.dart_tool/`
- CMake/ASAN 임시 빌드 폴더
- Python `__pycache__`
- 로컬에 내려받은 ARM64 패키지 캐시와 압축 해제본
- 로그와 임시 파일

ZIP에는 소스뿐 아니라 현재 로컬에 있는 실행 바이너리, IPK, SSH host key와 배포 설정도 포함한다.
다만 현재 운영 SQLite DB와 MQTT password file은 Pi4 내부에만 있고 2026-10-20 현재 Pi4가
연결되지 않아 별도 복사하지 못했다. 팀이 물리 Pi4를 그대로 이어받으면 해당 파일은 Pi4에 유지된다.
Wi-Fi/MQTT 평문 비밀번호는 이 작업 폴더에 원래 저장되어 있지 않으므로 ZIP에 추가할 파일이 없다.

## 3-1. 소스 코드가 실제로 있는 위치와 추가 코드 가능성

### 이 ZIP에 들어 있는 코드

현재 이 작업에서 작성한 다음 소스는 ZIP에 전부 포함되어 있다.

- Pi4 C++ HomeMind Core
- Flutter UI와 MQTT 연결 코드
- SQLite 스키마·마이그레이션·검증 코드
- MQTT/JSON 계약과 공식 장치 ID
- 가짜 ESP32 simulator, selftest, soaktest
- 빌드·설치·상태 확인 스크립트
- systemd, Mosquitto, Atlas IPK 패키징 파일

따라서 이 작업 폴더 안에 있으면서 ZIP에서 실수로 빠진 대량의 소스가 따로 있는 것은 아니다.

### Pi4 안에 있는 것

물리 Pi4에는 일반적으로 `core.cpp`나 Flutter 프로젝트 같은 원본 소스가 아니라 다음 운영 파일이 있다.

```text
/data/share/homemind/bin/       컴파일된 실행 프로그램
/data/share/homemind/lib/       실행 라이브러리
/data/share/homemind/config/    배포된 설정과 MQTT password file
/data/share/homemind/db/        현재 SQLite 운영 데이터
/etc/systemd/system/            자동 시작 서비스
```

즉, ZIP이 작아 보이는 이유가 소스 대부분이 Pi4에 숨겨져 있기 때문은 아니다. Pi4의 바이너리는
ZIP에 포함된 소스를 컴파일한 실행본이다. 현재 Pi4가 연결되지 않아 배포된 바이너리와 로컬 소스의
최종 해시 비교는 아직 하지 못했다.

### 별도의 코드가 존재할 수 있는 경우

다음 코드는 이 ZIP 밖의 다른 팀원 PC, Google Drive, GitHub 또는 메신저 파일에 있을 수 있다.

- 다른 팀원이 별도로 작성한 ESP32·ESP8266 펌웨어
- 다른 팀원이 작성한 Pi5 카메라·AI 비전 코드
- 별도 브랜치에서 수정한 Flutter UI
- 하드웨어 단품시험용 Arduino `.ino` 또는 PlatformIO 프로젝트
- 팀원이 개인적으로 만든 GPIO·센서 테스트 코드

이 코드들은 현재 작업 폴더에서 작성한 것이 아니므로 자동으로 ZIP에 포함될 수 없다. 팀원 전원에게
각자 보유한 코드, Git 저장소, Drive 링크가 있는지 확인하고 발견되면 하나의 저장소로 합쳐야 한다.

### 아직 존재하지 않는 코드

다음은 누락된 파일이 아니라 실제 장비가 준비된 뒤 새로 작성해야 하는 코드다.

- ESP32·ESP8266 실물 센서/액추에이터 펌웨어
- Octopus XY 모터 실물 펌웨어
- Pi5 V4L2 카메라 사람·인형 감지 코드
- 학습 데이터셋과 학습된 비전 모델
- 실제 GPIO 핀맵에 맞춘 드라이버·보정 코드

### 전체 코드 확인 절차

1. 이 ZIP의 `docs/CODE_INVENTORY_KO.md`를 읽는다.
2. 팀원 모두에게 별도 GitHub, Drive, 개인 PC 코드가 있는지 묻는다.
3. Pi4를 연결해 `/data/share/homemind/`의 바이너리·설정·DB를 백업한다.
4. 배포된 `homemind-core` 해시와 ZIP의 `dist/bin/homemind-core` 해시를 비교한다.
5. 다른 버전이 발견되면 실행 파일만 복사하지 말고 그 실행 파일을 만든 소스 저장소를 찾는다.
6. 모든 코드를 하나의 팀 저장소에 합치고 담당자와 버전을 표시한다.

### 팀원의 GPT가 먼저 읽어야 하는 순서

팀원이 ChatGPT나 다른 코딩 도구를 사용할 때 다음 순서로 파일을 제공한다.

1. `README_TEAM_HANDOFF_KO.md`
2. `docs/CODE_INVENTORY_KO.md`
3. 해당 담당 문서
   - ESP32: `docs/ESP32_HANDOFF_KO.md`
   - UI: `docs/UI_BACKEND_CONTRACT_KO.md`
   - 통합: `docs/INTEGRATION_CHECKLIST_KO.md`
4. `contracts/device-registry.json`
5. `contracts/protocol.json`
6. 작업과 관련된 JSON Schema

GPT에 다음 원칙도 함께 전달한다.

```text
- 기존 공식 ID, MQTT 토픽, JSON 필드명을 임의로 변경하지 않는다.
- Pi4에 대량의 미공유 원본 소스가 있다고 가정하지 않는다.
- 새 센서나 장치를 추가하면 contracts를 먼저 수정하고 검증한다.
- ESP32와 Pi5 비전 코드는 아직 구현 대상이라는 전제에서 작업한다.
- 다른 팀원 저장소가 발견되면 현재 ZIP과 비교한 뒤 통합한다.
```

## 4. 확정된 네트워크와 ID

### 개발 네트워크

| 용도 | 주소 |
| --- | --- |
| Pi4 직접 랜선 주소 | `10.10.10.2` |
| MQTT TCP 개발 포트 | `10.10.10.2:1883` |
| Flutter 웹 WebSocket | `ws://10.10.10.2:9001` |
| ESP32·Pi5 Wi-Fi MQTT | `Pi4의 Wi-Fi IP:1884` |

포트 `1883`과 `9001`은 격리된 PC–Pi4 개발 연결용이다. 일반 Wi-Fi의 ESP32와 Pi5는
인증이 설정된 `1884`를 사용한다. 팀에서 이미 공유한 Wi-Fi/MQTT 계정 값을 그대로 사용한다.

### 주요 노드 ID

| 장치 | 공식 node ID | 역할 |
| --- | --- | --- |
| 현재 Pi4 | `NODE-RPI4-01` | Core·MQTT·SQLite |
| 최종 Pi5 | `NODE-RPI5-01` | UI·비전 인식 |
| ESP32 | `CTRL-ESP32-ACT-01` | 서보·화장실 환기 팬 |
| ESP8266 | `CTRL-ESP8266-LED-01` | LED·FND |
| Octopus | `CTRL-OCTOPUS-01` | XY 이동장치·원점 센서 |

모든 공식 장치 ID는 `contracts/device-registry.json`이 원본이다. 임시 ID를 새로 만들지 않는다.

## 5. 핵심 통신 규칙

| 항목 | 확정값 |
| --- | --- |
| 스키마 | `pfn.v1` |
| command/ACK/Task/UI 행동 | QoS 1 |
| telemetry/heartbeat/health | QoS 0 |
| retained | 항상 false |
| heartbeat | 5초마다 |
| offline | 마지막 메시지 이후 15초 |
| ACK 제한 | 3초 |
| 최대 시도 | 3회 |
| command TTL | 10초 |
| 시간 | UTC Unix epoch millisecond |

주요 토픽:

```text
pfn/v1/device/{node_id}/command
pfn/v1/device/{node_id}/ack
pfn/v1/device/{node_id}/telemetry
pfn/v1/system/heartbeat
pfn/v1/ai/perception
pfn/v1/core/task/request
pfn/v1/core/task/update
pfn/v1/ui/action
pfn/v1/core/state/request
pfn/v1/core/state/response/{client_id}
pfn/v1/system/safety/alert
pfn/v1/ui/safety/ack
pfn/v1/ui/privacy/set
pfn/v1/system/privacy/state
pfn/v1/ui/feedback
```

정확한 필드와 예제는 `contracts/`의 JSON Schema를 사용한다. 토픽이나 필드명을 한 프로그램에서만
변경하면 안 된다.

## 6. 팀원별 다음 작업

### ESP32·ESP8266 담당자

가장 먼저 `docs/ESP32_HANDOFF_KO.md`를 읽는다. 이 문서에는 보드별 공식 ID, JSON 예제,
heartbeat, telemetry, command, 중복·만료 ACK, 실제 시나리오, 합격 기준이 모두 들어 있다.

필수 결과:

1. 실제 GPIO 핀맵 확정
2. 센서·LED·FND·서보·화장실 환기 팬 단품시험
3. heartbeat와 telemetry 발행
4. command를 실제 장치에 적용한 뒤 ACK 반환
5. 동일 command 중복 실행 방지
6. 전원 차단 15초 후 offline, 재부팅 후 자동 복귀
7. 세 시나리오 실물 통합시험

현재 등록되지 않은 센서 이름은 Pi4가 거절한다. 새 센서를 달면 먼저 공식 device ID, reading 이름,
자료형, 범위, 담당 node와 위치를 합의하고 `device-registry.json`과 `sensor-rules.json`을 함께 변경한다.

### Pi5·비전 담당자

1. `NODE-RPI5-01`에서 Flutter Atlas 앱 빌드
2. Wayland 1024×600 전체 화면과 터치 확인
3. 앱 메뉴 등록과 부팅 자동 실행
4. V4L2 실내 카메라 `CAM-INDOOR-01` 연결
5. 현관 카메라 `CAM-OUTDOOR-01` 연결
6. 사람/인형의 존재·위치·상태 인식
7. 영상 프레임이 아닌 감지 결과만 `pfn/v1/ai/perception`으로 전송
8. 카메라와 UI 동시 실행 시 성능시험

카메라 원본과 음성 원본을 MQTT나 Pi4 SQLite에 저장하지 않는다.

### Flutter UI 담당자

1. Pi5 실물에서 현재 UI 재빌드
2. 실제 터치와 화면 잘림 확인
3. ESP32 실제 센서값과 ACK 표시 확인
4. P0 화면과 개인정보 스위치 실물시험
5. 테스트 중 쌓인 `selftest` Task의 표시 정리
6. 최종 발표 문구·색상·간격 다듬기

### Pi4 담당자

1. 실장비 조립 전 서비스와 DB 상태 재확인
2. ESP32가 사용할 Wi-Fi IP와 이미 공유된 MQTT 1884 계정 확인
3. 새 센서가 있다면 계약 파일 업데이트·검증·재배포
4. 실제 ESP32 telemetry와 ACK의 SQLite 저장 확인
5. Pi4 재부팅 후 서비스·DB 유지 확인
6. 최종 장시간 통합시험

## 7. 노트북에서 Flutter UI 실행

Pi4가 연결된 실제 미리보기:

```powershell
cd homemind_flutter_test
.\RUN_PREVIEW.cmd
```

Pi4 없이 가짜 화면만 확인:

```powershell
cd homemind_flutter_test
.\RUN_LAPTOP_PREVIEW.cmd
```

브라우저에서 다음 주소를 연다.

```text
http://127.0.0.1:8765
```

화면 위에 `Pi4 Core 연결`이 표시되면 실제 Pi4와 연결된 상태다. `localhost`는 UI 파일을 노트북에서
여는 주소이고, 데이터는 별도로 `ws://10.10.10.2:9001`을 통해 Pi4에서 받는다.

Flutter 분석과 자동 테스트:

```powershell
cd homemind_flutter_test
.\CHECK_APP.cmd
```

## 8. Pi4 상태 확인

노트북과 Pi4를 직접 랜선으로 연결한 뒤:

```powershell
ssh root@10.10.10.2
```

Pi4에서:

```sh
systemctl is-active homemind-core homemind-mqtt dropbear.socket
```

세 줄 모두 `active`여야 한다.

노트북의 자동 상태 확인 파일:

```powershell
.\CHECK_PI4_STATUS.cmd
```

운영 파일 위치:

```text
프로그램: /data/share/homemind/bin/
라이브러리: /data/share/homemind/lib/
DB: /data/share/homemind/db/homemind.db
설정: /data/share/homemind/config/
서비스: /etc/systemd/system/homemind-core.service
```

Pi4의 전원을 뽑기 전에는 다음을 사용한다.

```sh
systemctl poweroff
```

SSH 연결이 닫힌 뒤 화면과 상태 LED를 확인하고 전원을 분리한다.

## 9. 소스 검증

계약과 공식 ID 검사:

```powershell
python tests\validate-contracts.py
```

정상 결과:

```text
CONTRACT_VALIDATION_PASS
```

Flutter 검사:

```powershell
homemind_flutter_test\CHECK_APP.cmd
```

실장비 통합시험 순서:

1. Pi4 세 서비스 `active`
2. ESP32 heartbeat와 online 확인
3. 정상 telemetry 저장
4. LED/팬/서보/FND command와 정상 ACK
5. 같은 command ID 재전송과 `duplicate: true` ACK
6. 만료 command와 `rejected_expired` ACK
7. 전원 차단 15초 후 offline
8. 재부팅 후 자동 online 복귀
9. 세 시나리오 완료
10. Pi4 재부팅 후 서비스와 SQLite 기록 유지

## 10. 알려진 결정 사항과 남은 위험

- 실제 서보 모델명이 기존 자료의 `BG90S`와 BOM의 `MG90S`로 다르므로 실물 각인을 확인한다.
- FND 세 개의 공식 ID는 정했지만 최종 GPIO와 담당 제어기는 배선 전에 확정한다.
- `FAN-01`은 거실 팬이 아니라 **화장실 환기 팬**이다.
- Pi4에는 RTC 배터리가 없어 인터넷 없이 재부팅하면 벽시계가 틀릴 수 있다. 데이터는 삭제하지 않고
  시간 미확인으로 표시하도록 구현되어 있다.
- 현재 개발용 1883/9001 연결은 최종 보안 구성이 아니다. 최종 Wi-Fi에서는 인증된 1884를 사용한다.
- QR 페어링, 모바일 원격 접속, 외부 카메라 스트림은 MVP 후순위다.
- 실제 카메라 인식과 실물 센서/액추에이터 통합은 장비가 준비된 뒤 최종 완료한다.

## 11. 계정과 운영 데이터 인수인계

팀원은 이미 공유받은 다음 값을 사용한다.

- Wi-Fi SSID와 비밀번호
- MQTT 사용자 이름과 비밀번호

이 값들의 평문 파일은 현재 작업 폴더에 존재하지 않으므로 패키지에 새로 넣지 않았다. Pi4 내부의
`/data/share/homemind/config/mosquitto.passwd`와 운영 DB는 물리 Pi4에 유지된다. Pi4를 팀에 넘긴 뒤
바로 사용할 수 있으며, 별도 백업본이 필요하면 Pi4를 네트워크에 연결한 상태에서 다음 파일을 복사한다.

```text
/data/share/homemind/config/
/data/share/homemind/db/homemind.db
/data/share/homemind/bin/
/data/share/homemind/lib/
/etc/systemd/system/homemind-*.service
```

개인 Windows 계정, 개인 SSH 개인키, HomeMind와 관계없는 브라우저·클라우드 인증정보는 팀 업무와
관계가 없으므로 포함하지 않는다.

## 12. 권장 작업 순서

```text
1. 각 담당자가 이 문서와 자기 역할 문서를 읽음
2. 공식 ID와 핀맵 확정
3. ESP32 센서·액추에이터 단품시험
4. Pi5 카메라 단품시험
5. Pi4 ↔ ESP32 MQTT/ACK 통합
6. Pi4 ↔ Pi5 비전/Flutter 통합
7. 세 시나리오 전체 실행
8. 오류·오프라인·재부팅 시험
9. 장시간 안정성 시험
10. 최종 발표용 시연 순서 확정
```

## 13. 변경 관리 원칙

- 공식 ID, MQTT 토픽, JSON 필드를 임의 변경하지 않는다.
- 계약 변경은 `contracts/`를 먼저 수정하고 모든 담당자가 같은 버전을 사용한다.
- 변경 후 `python tests/validate-contracts.py`를 반드시 실행한다.
- 소스, 계약, 문서, 펌웨어 버전을 함께 기록한다.
- 문제가 발생하면 Task ID, trace ID, command ID, node ID를 함께 남긴다.

이 문서와 ZIP의 버전을 한 묶음으로 보관한다. 이후 수정본을 전달할 때는 날짜나 버전 번호를 올려
구버전과 혼동되지 않게 한다.
