# PHONE-APP · 휴대폰 앱 (Android)

HomeMind TaskFlow 휴대폰 앱(Flutter). 기기 목록·제어, 작업 상태(처리 중·완료) 표시를 한다. 백엔드는
NODE-RPI4-01의 `homemind-mobile-api`(C++, API 0.4)이고, 앱은 QR 페어링으로 받은 토큰으로만 접속한다.
앱 소스: `team/homemind-application-2026-10-21/`.

| 들어가는 것 | 파일 (`payload/`) | 비고 |
|---|---|---|
| 앱 | `homemind-app-release.apk` (약 62 MB) | Flutter debug 키로 서명(시연용 직접 설치. 스토어용 아님) |

## 1. APK 만들기 (PC)
`bash deploy/PHONE-APP/build-apk.sh` → 앱 시험 후 release APK를 `payload/`에 넣고 SHA-256을 적는다.
경로 기본값: Flutter `C:\Users\alexi\dev\flutter`, Android SDK `C:\Android\sdk`, JDK 17 `C:\Android\jdk\...`
(다르면 `FLUTTER`, `ANDROID_HOME`, `JAVA_HOME`으로 지정. Flutter·Android Studio 설정은 바꾸지 않는다).

## 2. 휴대폰에 설치 (사람)
- USB: 휴대폰 개발자 옵션 → USB 디버깅 켜기 → `C:\Android\sdk\platform-tools\adb.exe install -r deploy\PHONE-APP\payload\homemind-app-release.apk`
- 또는 APK 파일을 휴대폰으로 복사해 열기(휴대폰에서 "출처를 알 수 없는 앱" 설치 허용이 필요)

## 3-A. Pi 4 없이 PC로 시험 (시뮬레이션)
PC 데모 스택(`docker compose -f demo/pc/docker-compose.yml --profile fake-esp up -d`: 브로커 + core + 가짜 ESP)에
Pi 4와 같은 C++ Mobile API를 붙인다. 휴대폰과 PC는 같은 공유기에 있어야 한다.
1. `bash deploy/PHONE-APP/pc-mobile-api.sh <PC IP>` (예: `ipconfig`의 Wi-Fi IPv4) → 빌드·실행 후 터미널에 QR
2. 앱 → 기기 연결 → QR 스캔 → 앱에 보이는 코드와 터미널의 `pairing code`가 같은지 확인
3. 기기 제어 → 가짜 ESP가 ACK → 앱에 완료 표시. 현관문 열기는 7인치 UI 승인이 필요한 명령이라 "승인 대기"로 남는다
   (앱 **승인** 탭에 보이지만 휴대폰으로는 거절만 된다). AI 제안 등 다른 승인 대기는 앱 승인 탭에서 승인·거절, 새 요청은 앱이 켜져 있으면 바로 배너로 알림
4. 새 QR: `pc-mobile-api.sh --qr`(일회용, 10분) · 끝내기: `pc-mobile-api.sh --stop`
5. 기기와 같은 HTTPS로 시험: `pc-mobile-api.sh <PC IP> --https`(자체 서명 인증서, QR에 지문 → 앱이 그 인증서만 믿음)
- 7인치 UI의 QR 경로 확인: `bash deploy/PHONE-APP/test-ui-qr-live.sh` → UI의 MQTT 데이터 소스가 QR을 요청하고(`pfn/v1/mobile/pairing/request`)
  C++ Mobile API가 일회용 코드로 응답, 그 QR로 휴대폰처럼 연결(201) → 기기 목록(200) → 같은 QR 재사용 거부(401). 시험 토큰은 폐기
- 휴대폰 없이 앱 통신 코드만 확인: `bash deploy/PHONE-APP/test-app-live.sh` → 앱의 `HomeMindApi`로 QR 페어링(일회용 확인),
  기기 목록, 거실 LED 켜기·끄기(core → 가짜 ESP → 완료), 현관문 열기 승인 대기 → 휴대폰 승인 403 → 거절 → rejected,
  실시간 이벤트(SSE) 수신, 토큰 없이 401. 시험 토큰은 끝나면 폐기
- 휴대폰이 접속하지 못하면: 같은 Wi-Fi인지, Windows 방화벽이 TCP 8000 수신을 허용하는지(사람이 설정) 확인

## 3-B. 실제 백엔드 (사용자 승인 후)
`bash deploy/NODE-RPI4-01/install-mobile-api.sh` (core 0.6.3 다음) → 기기에서 사람이 MQTT 비밀번호 파일을 만들고
**7인치 UI 더보기 → 휴대폰 연결**의 QR을 스캔(Pi4 터미널에서 `--print-pairing-qr`도 가능). 자세한 순서는 `deploy/NODE-RPI4-01/README.md`.

## PC 시험 결과 (시뮬레이션)
앱 시험 21 passed(analyze 0, 승인 탭·실시간 알림 포함), APK 빌드·서명(2026-10-22), Mobile API Python↔C++ 53/53 단계 동일(PC core E2E 포함),
`pc-mobile-api.sh`: C++ API가 PC 브로커에 연결되고 PC의 공유기 주소로 응답, 터미널 QR 출력 확인,
`test-app-live.sh`: 앱 통신 코드 ↔ C++ Mobile API ↔ PC core ↔ 가짜 ESP 통과.
**실제 휴대폰 설치·페어링은 아직.**
