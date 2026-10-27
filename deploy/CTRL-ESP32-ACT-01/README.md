# CTRL-ESP32-ACT-01 · 현관문 + 창문 (ESP32 DevKit)

펌웨어 `firmware/src/esp32_actuator` (PlatformIO env `esp32dev`). pfn.v1 MQTT: core 명령 → 실행 → ACK, heartbeat 5초, telemetry.

registry v4 (사용자 2026-10-25): MG90S 하나 고장 → 화장실 문 철거, 현관문·창문은 각도 제어가 안 되는 열기/닫기 모터.

| 장치 | 핀 | 비고 |
|---|---|---|
| SERVO-DOOR-01 현관문 | GPIO19 | 열기 = 스케치 `1,-90`, 닫기 = `1,0` |
| SERVO-WINDOW-01 창문 | GPIO21 | 열기 = 스케치 `2,-90`, 닫기 = `2,90` |
| SERVO-DOOR-02 화장실 문 | — | 철거, 명령은 `DEVICE_NOT_WIRED`로 거절 |

환기팬(FAN-01)은 ESP8266 D2로 옮겼다(`deploy/CTRL-ESP8266-LED-01`).

모터 신호는 사용자가 시험한 스케치와 같다: 50 Hz, 1000~2000 µs, `write(90 + offset)`, 켜질 때는 핀 LOW만(신호 없음),
첫 명령에서 PWM 시작. Pi 쪽은 각도를 모르고 열기(90)/닫기(0) 코드만 보낸다.

## 순서 (사용자 승인 후)
1. Wi-Fi·브로커 주소는 코드에 있다(`firmware/include/hm_site_network.h`, 사용자 결정 2026-10-25). `firmware/include/secrets.h.example` → `secrets.h` 복사 후 `CTRL-ESP32-ACT-01` 브로커 비밀번호만 **로컬에서** 채운다(커밋 금지)
2. 같은 비밀번호로 Pi4 브로커 계정 등록: `bash deploy/set-mqtt-password.sh esp32` (예전 계정은 Homebrew 2.1 형식이라 Pi4 브로커가 거부)
3. 보드를 노트북 USB에 연결 → `bash deploy/CTRL-ESP32-ACT-01/flash.sh <포트>` (Windows `COM5`, macOS `/dev/cu.usbserial-…`: `ls /dev/cu.*`). Windows PC에서는 논리 시험 → 빌드 → 업로드, 맥북·리눅스는 빌드 → 업로드(논리 시험은 Windows MinGW 전용, PC에서 통과)
4. 직렬 로그(115200)에서 Wi-Fi·MQTT 연결, heartbeat 확인 → 7인치 UI에서 창문 열기/닫기

## PC 시험 결과 (시뮬레이션)
registry v4: native 시험 71/71(문·축 합계), `esp32dev` 빌드 SUCCESS, 메시지 계약 점검 `FIRMWARE_MESSAGES_PASS`. **보드 실기 시험은 아직.**
상세: `firmware/README.md`
