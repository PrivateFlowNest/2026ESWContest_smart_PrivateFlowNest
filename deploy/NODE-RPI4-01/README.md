# NODE-RPI4-01 · 백엔드 (Raspberry Pi 4, LG AI Native OS Headless)

Mosquitto 브로커 + `homemind-core`(C++, SQLite). 이 폴더는 **core 0.6.4 설치**용(지금 실기 Pi4는 0.6.3, 2026-10-23 설치).

| 들어가는 것 | 파일 (`payload/`) | 비고 |
|---|---|---|
| core 0.6.4 | `homemind-core_0.6.4_aarch64.ipk` | 브로커·core 바이너리, systemd 유닛, 계약 설정 5개, ACL 템플릿(0.6.2: 7인치 UI 휴대폰 연결 QR 토픽, "상황이 달라요" 오탐 정정 / 0.6.3: 휴대폰 승인 토픽, 현관문 열기는 휴대폰 승인 거부 / 0.6.4: 승인 대기 만료 — 일반 10분, 현관문 2분) |
| 되돌리기용 | `homemind-core_0.6.3_aarch64.ipk`, `homemind-core_0.5.2_aarch64.ipk` | `opkg install --force-downgrade` |
| 되돌리기용 0.5.2 | `homemind-core_0.5.2_aarch64.ipk` | 문제 시 `opkg install --force-downgrade` |
| 휴대폰 Mobile API | `homemind-rpi4-mobile-api_0.2.1_aarch64.ipk` (C++, Pi 4 Cortex-A72 빌드, API 0.5) + 브로커 ACL `mosquitto.acl` | core 0.6.3 이후 `install-mobile-api.sh`(1b단계: 기기 ACL이 core 원본이면 휴대폰 목록 토픽이 든 ACL로 바꾸고 재시작 없이 다시 읽기). HTTP 8000, QR 페어링(터미널 또는 7인치 UI 더보기 → 휴대폰 연결) + 토큰, 휴대폰 승인·실시간 알림(SSE), 7인치 더보기 → 연결된 휴대폰(목록·연결 해제) |

## 순서 (사용자 승인 후)
1. PC: `bash deploy/collect.sh NODE-RPI4-01` → `payload/` 생성(해시 확인)
2. PC: `bash deploy/NODE-RPI4-01/install.sh root@<Pi4 주소>`
   - 운영 DB 온라인 백업(덮어쓰지 않음) → IPK 복사·설치 → 설정 버전 확인 → 서비스·로그 확인
   - SSH 호스트 키는 팀 파일 `team/homemind-core-ui-2026-10-20/.pi4_known_hosts`로 고정 확인
3. 확인: `systemctl is-active homemind-core homemind-mqtt`, 로그에 `CORE_STARTED`, `MQTT_CONNECTED`
4. (휴대폰 앱을 쓸 때) PC: `bash deploy/NODE-RPI4-01/install-mobile-api.sh root@<Pi4 주소>` → 기기에서 사람이
   `homemind-mobile-api` 브로커 계정과 비밀번호 파일을 만들고, `--print-pairing-qr`로 터미널에 QR을 띄워 앱으로 스캔
   (인증 리스너 1884가 켜져 있어야 함. 휴대폰 연결은 HTTPS: 첫 시작 때 기기에서 자체 서명 인증서를 만들고 QR에 지문을 넣어
   앱이 그 인증서만 믿는다. 인증서 파일을 지우면 휴대폰은 새 QR로 다시 연결)

## 브로커 잠그기 (공유기 연결 후, 사용자 승인 후)
`bash deploy/NODE-RPI4-01/secure-broker.sh` (맥북: 먼저 `brew install mosquitto` — 기기에는 `mosquitto_passwd`가 없다)
- 공유기 내부망이 `10.10.10.0/24`라 예전 직결용 익명 `10.10.10.2:1883`이 Wi-Fi까지 열려 있었다 → 1883은 `127.0.0.1`(core만),
  9001 제거, 1884는 비밀번호 + `mosquitto.acl`. core는 drop-in으로 `127.0.0.1:1883`(패키지 업그레이드에도 유지)
- 계정 5개(NODE-RPI5-01, NODE-RPI5-02, homemind-mobile-api, CTRL-ESP32-ACT-01, CTRL-ESP8266-LED-01): 사람이 맥북에서
  비밀번호를 입력(화면에 안 보임), 해시만 기기로. 같은 비밀번호를 나중에 각 기기 비밀번호 파일·`secrets.h`에 넣는다
- 백업 `backup/broker-<시각>`, 브로커가 안 뜨면 예전 설정으로 자동 복구. 예전 계정(`homemind-esp32`, `homemind-pi5`)은 지우지 않음(ACL에 없어 권한 없음)

## 사람이 할 일
- **MQTT 계정·ACL 활성화(선택)**: 인증 리스너(1884)를 쓰려면 기기에서 `mosquitto_passwd`로 계정을 만든다. 사용자명 = 노드 ID
  (`NODE-RPI4-01`, `NODE-RPI5-01`, `NODE-RPI5-02`, `CTRL-ESP32-ACT-01`, `CTRL-ESP8266-LED-01`, `homemind-mobile-api`).
  ACL 템플릿은 설치 후 `/data/share/homemind/config/mosquitto.acl`. 비밀번호는 어디에도 적지 않는다
- 공유기 고정 IP를 정한 뒤 RPI5·ESP 설정에 이 주소를 넣는다 ([미정])
- 상세: `team/homemind-core-ui-2026-10-20/docs/RELEASE_0.6.0_KO.md`, `RELEASE_0.6.1_KO.md`, `RELEASE_0.6.2_KO.md`, `RELEASE_0.6.3_KO.md`, `RELEASE_0.6.4_KO.md`

## PC 시험 결과 (시뮬레이션)
x86 PC 시험 PASS(selftest·마이그레이션·soak), ACL 16/16(실제 Mosquitto), arm64 IPK 설치 상태 core + selftest PASS(QEMU). **Pi4 실기 설치는 아직.**
