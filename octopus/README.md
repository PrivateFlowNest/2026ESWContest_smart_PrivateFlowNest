# CTRL-OCTOPUS-01 Klipper 설정 (P6)

BTT Octopus V1.1 + Klipper, 호스트 NODE-RPI4-02(Ubuntu). 인형을 XY로 움직이기만 하고 HomeMind MQTT와 연동하지 않는다(system-architecture 3-4).
핀은 `docs/handoff/reference/electrical-harness-plan.md`(확정)를 그대로 옮겼다.

| 축 | 장치 ID | 드라이버 슬롯 | STEP | DIR | EN | TMC2209 UART | 원점 센서 |
|---|---|---|---|---|---|---|---|
| X | MOTOR-XY-X-01 | MOTOR0 | PF13 | PF12 | PF14(반전) | PC4 | SENSOR-HOME-X-01, J27 PG6 |
| Y | MOTOR-XY-Y-01 | MOTOR1 | PG0 | PG1 | PF15(반전) | PD11 | SENSOR-HOME-Y-01, J29 PG9 |

## 두 가지 설정 (시연 이동 방식은 [미정], 사용자 결정)

| 파일 | 이동 방법 | 특징 |
|---|---|---|
| `klipper/printer-cartesian.cfg` | `G28`, `G0 X.. Y..`, 매크로 `MOVE_DOLL X=80 Y=40`, `PARK_DOLL` | 일반 G-code(수동 입력·Mainsail/Fluidd 버튼) 사용 가능. Klipper가 Z축을 요구해서 빈 MOTOR2 슬롯에 자리만 둠(연결 없음, 원점 복귀 안 함) |
| `klipper/printer-manual-stepper.cfg` | 매크로 `DOLL_HOME`, `DOLL_MOVE X=80 Y=40 SPEED=30` | 가짜 Z 없음. G0/G1은 안 됨. 스크립트·매크로로만 움직일 때 |

쓰는 쪽 파일을 Klipper 호스트의 `~/printer_data/config/printer.cfg`로 복사한다.

## 모형에서 확인할 값 (설정 파일의 `TODO`)
- `[mcu] serial`: 펌웨어를 올린 뒤 `ls /dev/serial/by-id/`
- 원점 센서 극성: `QUERY_ENDSTOPS`를 센서를 가린 상태/연 상태로 실행해 `TRIGGERED`가 맞게 나오도록 `^PG6`의 `!` 추가·제거
- 이동 거리 `position_max`(지금 200 mm 자리값), `rotation_distance`(GT2 2 mm × 20T 풀리 = 40 가정)
- 모터 전류 `run_current`(0.6 A에서 시작)
- **GT2 벨트가 BOM에서 "구매 안 함"** → 벨트가 없으면 XY 시험 불가(사람 확인)

## Klipper 펌웨어 빌드 (Octopus V1.1)
`make menuconfig`: Enable extra low-level configuration options, STM32, 칩은 보드에 적힌 것(STM32F446 → 12 MHz crystal / STM32F429 → 8 MHz crystal), Bootloader offset 32KiB, USB on PA11/PA12.
`klipper.bin`을 microSD에 `firmware.bin`으로 넣고 전원을 켠다. 칩이 F429이면 `[mcu] serial`의 `stm32f446xx`도 바뀐다.

## PC 검사 (시뮬레이션, 보드 없음)
Klipper batch 모드로 두 설정을 실제 Klipper(STM32F446 사전 파일)에 넣고 시험 G-code(`test/*.gcode`)를 돌린다.
```bash
docker build -t homemind-klipper-check -f octopus/klipper/test/klipper-check.Dockerfile octopus/klipper/test
docker run --rm -v "$PWD/octopus/klipper:/cfg" homemind-klipper-check bash /cfg/test/run-batch-check.sh
```
2026-10-22 결과: 두 파일 모두 `BATCH_OK`(설정 해석, 매크로, 원점 복귀·이동 G-code가 MCU 명령으로 변환됨, 경고 0). **보드·모터 실기 시험은 아님.**
