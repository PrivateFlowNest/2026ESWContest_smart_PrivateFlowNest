# CTRL-ESP8266-LED-01 · 거실 조명 + 에어컨 표시등 + 환기팬 (ESP8266 NodeMCU v2)

펌웨어 `firmware/src/esp8266_led` (PlatformIO env `nodemcuv2`). 점등 = 전원, 느린 깜빡임 = 조용 모드.
registry v4 (사용자 2026-10-25, 시험한 스케치와 같은 핀): HIGH = 켜짐, 켜질 때 모두 LOW.

| 장치 | 핀 | 사용자 스케치 |
|---|---|---|
| LED-LIVING-01 거실 조명 | D0 (GPIO16) | `3,1` / `3,0` |
| LED-AIRCON-01 에어컨 표시등 | D1 (GPIO5) | `4,1` / `4,0` |
| FAN-01 화장실 환기팬 | D2 (GPIO4) | `5,1` / `5,0` |

세탁기·TV·노트북 LED, 야간 안내등, FND는 모형에 없다(명령은 `DEVICE_NOT_WIRED`로 거절).

## 순서 (사용자 승인 후)
1. Wi-Fi·브로커 주소는 코드에 있다(`firmware/include/hm_site_network.h`). `firmware/include/secrets.h`에 `CTRL-ESP8266-LED-01` 비밀번호를 로컬에서 채운다(ESP32와 같은 파일)
2. 같은 비밀번호로 Pi4 브로커 계정 등록: `bash deploy/set-mqtt-password.sh esp8266`
3. `bash deploy/CTRL-ESP8266-LED-01/flash.sh <포트>` (Windows `COM6`, macOS `/dev/cu.usbserial-…`) → 직렬 로그 확인 → UI에서 거실 조명 켜기

## PC 시험 결과 (시뮬레이션)
registry v4: native 시험 71/71(공용), `FIRMWARE_MESSAGES_PASS`, `nodemcuv2` 빌드 SUCCESS. **보드 실기 시험은 아직.** 상세: `firmware/README.md`
