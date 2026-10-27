# 기기별 설치 (deploy/)

기기마다 폴더 하나. 각 폴더에 **README(무엇을·어떤 순서로) + 설치/굽기 스크립트 + payload/(설치 파일)**가 있다.
`payload/`는 git에 없고 PC에서 `bash deploy/collect.sh all`로 만든다(IPK·모델을 해시 확인 후 복사). 그 뒤에는 기기 폴더만 들고 가면 된다.

| 폴더 | 기기 | 설치하는 것 | 방법 |
|---|---|---|---|
| `NODE-RPI4-01/` | Pi 4 · 백엔드 | homemind-core 0.6.3 (Mosquitto + core + SQLite), 휴대폰 Mobile API | `install.sh` (SSH, DB 백업 후 opkg) |
| `NODE-RPI5-01/` | Pi 5 · 7인치 UI + EXAONE | homemind-rpi5-llm, homemind-rpi5-ui, EXAONE v3 GGUF | `install.sh` (SSH) |
| `NODE-RPI5-02/` | Pi 5 · 인식(마이크·카메라) | homemind-rpi5-perception, Whisper 모델 (+선택 Vosk) | `install.sh` (SSH) |
| `PHONE-APP/` | 휴대폰 (Android) | HomeMind 앱 APK (Flutter) | `build-apk.sh`(PC 빌드) → USB `adb install` 또는 파일 복사. Pi4 없이 시험: `pc-mobile-api.sh <PC IP>` |
| `CTRL-ESP32-ACT-01/` | ESP32 · 서보 3 + 팬 | 펌웨어 `esp32dev` | `flash.sh COMx` (USB) |
| `CTRL-ESP8266-LED-01/` | ESP8266 · LED 5 | 펌웨어 `nodemcuv2` | `flash.sh COMx` (USB) |
| `DOLL-AXIS/` | 인형 앞뒤 1축 모터 (HomeMind 연동 안 함) | Teensy 4.1 또는 ESP32 펌웨어 `teensy41_axis`/`esp32_axis` | `flash.sh teensy|esp32` (USB), 조작은 `firmware/tools/doll_axis_cli.py` |
| `NODE-RPI4-02/` | Pi 4 + Octopus | **보류**(1축 계획으로 대체 검토) | — |

## 전체 순서 (모두 사용자 승인 후)
0. 공유기 설치 → RPI4-01·RPI5-01·RPI5-02 고정 IP를 `deploy/site.env`(틀: `site.env.example`, git 제외)에 적기 → 이후 설치 스크립트는 인자 없이 실행 가능. 두 Pi5 시간 동기(NTP)
1. **RPI4-01**: core 0.6.3 업그레이드 → MQTT 계정(노드 ID)·비밀번호는 기기에서 사람이 생성
2. **ESP32·ESP8266**: `firmware/include/secrets.h`를 로컬에서 채우고 굽기 → heartbeat 확인
3. **RPI5-02**: 인식 설치 → 비밀번호 파일 → 마이크 확인
4. **RPI5-01**: LLM + UI 설치 → 비밀번호 파일 → 7인치 화면 확인
4-1. **휴대폰**: APK 설치 → RPI4-01에 Mobile API 설치(`NODE-RPI4-01/install-mobile-api.sh`) → 7인치 UI 더보기 → 휴대폰 연결의 QR(또는 Pi4 터미널 QR)로 페어링
5. **DOLL-AXIS**: 보드·드라이버·레일이 정해진 뒤 핀과 `steps_per_mm`을 채우고 굽기 → 원점·위치 보정

비밀번호·Wi-Fi 값은 어떤 파일·문서·로그에도 쓰지 않는다(기기의 비밀번호 파일, 로컬 `secrets.h`만).

## 다른 노트북(맥북)에서 설치 (2026-10-22)
빌드·서명은 이 PC에서만 하고, 현장 노트북은 서명된 설치 파일을 보내기만 한다(Docker·개인 키 불필요).
1. PC: 커밋 후 `bash deploy/pack-field-kit.sh` → `~/Desktop/HomeMind-field-kit/`(코드 git bundle, 서명된 payload tar, `site.env`, 순서 안내, 해시)
2. USB로 맥북에 복사 → 안내 파일(`README-설치순서.txt`) 순서대로: `brew install openssl@3` → 묶음 해시 확인 → clone·tar 풀기 → `bash deploy/preflight.sh --ssh`
- 맥에서 다른 점(스크립트가 처리): LibreSSL 대신 Homebrew `openssl@3`, `sha256sum` 없으면 `shasum`, scp가 SFTP로 실패하면 예전 방식(`-O`)으로 다시,
  설치 한 번에 SSH 로그인 한 번(연결 재사용). bash 3.2로 동작(4 전용 문법 없음)
- 네트워크: 공유기 `10.10.10.1`, DHCP `.100`부터, Pi4 `.2`(패키지가 고정), Pi5-01 `.3`·Pi5-02 `.4`(예약 권장), Pi는 유선.
  맥북 USB-C LAN이 주소를 못 받으면 `sudo ipconfig set en7 MANUAL 10.10.10.50 255.255.255.0`
- **시간**: Pi에는 시계 배터리가 없다(실기 Pi4가 켜진 뒤 9월 1일, 52일 늦음). 켤 때마다 `bash deploy/sync-time.sh`로 노트북 시간을 넣는다
  (1분 넘게 틀렸던 Pi는 HomeMind 서비스 재시작). 시계가 틀리면 휴대폰 QR이 만료로 보이고 기록 시각이 틀어진다

## 릴리스 서명
- `collect.sh`가 각 기기 `payload/SHA256SUMS`를 ed25519 릴리스 키로 서명해 `SHA256SUMS.sig`를 만든다(`common/payload.sh`)
- 설치 스크립트는 기기에 접속하기 전에 **서명 → 모든 파일 해시** 순서로 확인하고, 서명이 없거나 틀리면 멈춘다
  (파일과 해시 목록을 함께 바꿔치기해도 서명에서 걸린다). PC 시험에서만 `HM_ALLOW_UNSIGNED=1`로 서명 없이 허용
- 개인 키: 이 PC의 `~/.homemind/homemind-release-ed25519.pem`(저장소 밖, 따로 백업). 공개 키: `keys/homemind-release-ed25519.pub`(커밋)
- 키 만들기·지문 보기: `bash deploy/release-key.sh init|show` (지문 sha256 앞 16자 `12105942e126db8d`)

## 시험 (PC, 시뮬레이션)
- 각 IPK: QEMU arm64에서 설치 상태 시험 통과(`native/packaging/test/`), PC E2E 10/10(`native/tests/test_e2e_pc_native.py`)
- 설치 스크립트 자체: `deploy/test/run-install-dryrun.sh` — SSH가 열린 가짜 기기 컨테이너(opkg·systemctl 흉내)에 실제로 설치해 순서·해시 확인·설정 치환을 점검
- **실기(Pi·ESP) 설치는 아직 한 번도 하지 않았다.**
