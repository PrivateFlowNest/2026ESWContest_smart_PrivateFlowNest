# HomeMind TaskFlow

제24회 임베디드SW경진대회 스마트 가전 부문 · 팀 PrivateFlowNest

말이 끝나기 전에 준비하고, 상황을 보고 먼저 제안하는 온디바이스 AI 스마트홈입니다. LG AI Native OS가 올라간 Raspberry Pi 5 두 대와 Raspberry Pi 4, ESP32·ESP8266이 MQTT로 연동되어 음성 인식, 숨은 의도 이해(EXAONE), 소리 상황 인지를 모두 집 안에서 처리합니다. AI는 제안만 하고, 실행은 Pi 4의 정책 검증과 사람의 승인을 거칩니다.

- 시연 동영상: 〈유튜브 링크〉
- 개발완료보고서 · 작품소개서: 대회 제출본

## 시스템 구성

| 기기 | 역할 | 코드 |
|---|---|---|
| Raspberry Pi 4 (NODE-RPI4-01) | MQTT 브로커(인증·ACL), core(검증·정책·작업), SQLite, 휴대폰 API | `team/homemind-core-ui-2026-10-20/src`, `native/services/homemind-mobile-api` |
| Raspberry Pi 5 (NODE-RPI5-02) | 음성 인식(Whisper), 부분 발화, 소리 상황, 영상 위치, 상황 융합 | `native/services/homemind-perception`, `homemind-vision`, `native/libs` |
| Raspberry Pi 5 (NODE-RPI5-01) | EXAONE 4.0 1.2B LoRA, 7인치 터치 UI | `native/services/homemind-llm`, `team/homemind-core-ui-2026-10-20/homemind_flutter_test` |
| ESP32 (CTRL-ESP32-ACT-01) | 현관문·창문 열기/닫기 모터 | `firmware/src/esp32_actuator` |
| ESP8266 (CTRL-ESP8266-LED-01) | 거실 조명 LED, 에어컨 표시등 LED, 화장실 환기팬 | `firmware/src/esp8266_led` |
| 휴대폰 (Android) | QR 페어링, 장치 상태·제어, 승인 요청 알림 | `team/homemind-application-2026-10-21` |

## 폴더

| 폴더 | 내용 |
|---|---|
| `contracts/` | 메시지 계약 `pfn.v1`(JSON Schema), 공식 장치 ID·정책·규칙, 계약 검증기 |
| `native/` | C++ 네이티브 서비스와 라이브러리(aarch64 IPK), 패키징, 시험 |
| `team/` | Pi 4 core(C++), 7인치 UI(Flutter), 휴대폰 앱(Flutter) |
| `firmware/` | ESP32·ESP8266 펌웨어(PlatformIO, Arduino) |
| `services/` | AI 모델 학습·평가 코드(EXAONE LoRA, Whisper, 소리 상황, 부분 발화, 영상 위치, 상황 융합) |
| `deploy/` | 기기별 설치 스크립트, 서명·수집, 설치 리허설 |
| `octopus/` | XY 이동 장치(Klipper) 설정 |
| `demo/` | PC에서 core·브로커를 띄우는 Docker 구성 |
| `docs/submission/` | 보고서·작품소개서·시연 동영상 생성 코드, `docs/ui-screenshots/` 화면 캡처 |

## 공개본에서 뺀 것

- 학습된 모델과 데이터 원본(EXAONE·Whisper 가중치 등): 용량이 커서 넣지 않았습니다.
- LG AI Native OS SDK·샘플과 교육 자료: 배포 권한이 없어 넣지 않았습니다.
- 비밀 값: MQTT 비밀번호는 `firmware/include/secrets.h`(예시: `secrets.h.example`)와 기기의 설정 파일에만 둡니다. 펌웨어의 Wi-Fi 값(`firmware/include/hm_site_network.h`)은 자리표시로 바꿔 두었습니다.
- 개발 기록 문서와 대회 양식.
