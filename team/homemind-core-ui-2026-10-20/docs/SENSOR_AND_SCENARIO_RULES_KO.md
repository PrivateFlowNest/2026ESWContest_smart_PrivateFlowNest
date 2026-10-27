# 센서 검증·시나리오 규칙 사용법

마지막 갱신: 2026-09-04  
Pi4 설치 버전: `homemind-core 0.5.2`

## 1. 센서값 처리 원칙

Pi4는 `contracts/sensor-rules.json`을 기준으로 센서 이름, 자료형, 범위와
센서 출처 ID를 검사한다.

| 센서값 | 허용 범위·형식 | 출처 장치 |
| --- | --- | --- |
| `sound_level_db` | 숫자 0~120 | `MIC-INDOOR-01` |
| `x_home_active` | 참/거짓 | `SENSOR-HOME-X-01` |
| `y_home_active` | 참/거짓 | `SENSOR-HOME-Y-01` |
| `fan_level_percent` | 숫자 0~100 | 화장실 환기 팬 `FAN-01` |
| `door_position_degrees` | 숫자 0~180 | 현관·욕실·세탁기 문 서보 |
| `window_position_degrees` | 숫자 0~180 | `SERVO-WINDOW-01` |
| `bed_curtain_position_degrees` | 숫자 0~180 | `SERVO-BED-CURTAIN-01` |
| `light_on` | 참/거짓 | 등록된 RGB·거실·이동 조명 |
| `display_value` | 정수 0~9999 | 세탁기·에어컨·TV FND |

등록되지 않은 센서 이름, 잘못된 자료형, 범위 초과, 출처 ID 불일치는 모두
정상 센서 데이터로 사용하지 않는다. 단, 문제 분석을 위해 받은 원본 MQTT
메시지와 거부 이유는 DB에 남긴다. 자동 삭제는 하지 않는다.

## 2. 현재 자동 시나리오

규칙 원본은 `contracts/scenario-rules.json`이다.

| 시나리오 intent | 실제 명령 |
| --- | --- |
| `prepare_home` | 거실 조명 켜기 → 화장실 환기 팬 30% |
| `prepare_sleep` | 거실 조명 끄기 → 화장실 환기 팬 끄기 → 침대 커튼 180도 |
| `night_bed_exit` | 이동 조명 켜기, 15초 후 자동 끄기 |

사용자가 UI에서 Task를 승인하거나 보류된 Task를 재개하면 Pi4가 해당 명령을
대상 제어기에 발행한다. 모든 명령의 ACK가 성공하면 Task는 `completed`,
하나라도 실패하면 `failed`가 된다.

## 3. 가짜 ESP32 사용

랩탑 PowerShell에서 실행한다.

```powershell
.\scripts\fake-esp32.ps1 -Profile normal -DurationSeconds 30
.\scripts\fake-esp32.ps1 -Profile mixed -DurationSeconds 30
.\scripts\fake-esp32.ps1 -Profile out-of-range -DurationSeconds 10
```

- `normal`: 정상 센서값만 전송
- `mixed`: 정상, 범위 초과, 잘못된 자료형, 알 수 없는 센서값을 순서대로 전송
- `out-of-range`: 범위를 벗어난 값만 전송
- `wrong-type`: 숫자 자리에 문자를 전송
- `unknown`: 등록되지 않은 센서 이름을 전송

## 4. 15초 오프라인 시험

```powershell
.\scripts\offline-test.ps1
```

가짜 ESP32가 heartbeat를 보내는 동안 `online`인지 확인하고, 송신 중단 후
17초를 기다려 `offline`인지 확인한다. 성공하면 `OFFLINE_TEST_PASS`가
표시된다.

## 5. 2026-09-01 실제 시험 결과

- Pi4 ARM64 설치 파일 버전: `0.5.2`
- 센서 범위·자료형·출처 검사: 통과
- 정상값 2건: telemetry 2건으로 저장
- 범위 초과 2건: 거부 이유와 원문 저장
- 잘못된 자료형 2건: 거부 이유와 원문 저장
- 알 수 없는 센서 2건: 거부 이유와 원문 저장
- 시나리오 명령 2개 → ACK → Task 완료: 통과
- heartbeat 중단 → 17초 후 offline: 통과
- Pi4 재부팅 후 MQTT·Core 자동 시작: 통과
- 기존 DB 무결성: `ok`, 자동 삭제 없음
