# DOLL-AXIS · 인형 앞뒤 1축 이동 (단독 제어, HomeMind와 연동 안 함)

2026-10-22 결정: 인형은 앞뒤 1축·모터 1개(NEMA17) + 광센서 1개로 움직인다. 인형은 "사람 역할"이라 HomeMind 기기가 아니므로
MQTT·core와 연결하지 않는다. 노트북(또는 Jetson·Pi)이 USB 시리얼로 명령한다. Octopus·RPI4-02는 쓰지 않는다.

| 항목 | 내용 |
|---|---|
| 보드 | Teensy 4.1(있으면) 또는 ESP32 DevKit — 펌웨어가 둘 다 빌드됨 (`teensy41_axis`, `esp32_axis`) |
| 펌웨어 | `firmware/src/doll_axis` + `firmware/lib/hm_axis`, 설정 `firmware/include/doll_axis_config.h` |
| 드라이버 | STEP/DIR/EN 입력 드라이버(예: TMC2209 단독 모드, A4988, DRV8825) — 미정 |
| 조작 | `python firmware/tools/doll_axis_cli.py --port <포트> HOME "MOVE bed"` |

## 명령 (USB 시리얼 115200, 한 줄에 하나)
`HOME`(광센서로 원점) · `MOVE <이름|mm>` · `STOP`(감속 정지) · `ESTOP`(즉시 정지·전원 끔) · `POS?` · `LIST` · `SET <이름> <mm>` · `STATUS?` · `HELP`.
응답 `OK ...` / `ERR <코드> ...`, 이벤트 `EVT HOMED`, `EVT ARRIVED <이름> <mm>`, `EVT ERROR <코드>`. 원점 전에는 이동을 거부한다(`ERR NOT_HOMED`).

## 보드가 정해지면 (사람 + 다음 작업)
1. `doll_axis_config.h`의 TODO 채우기: 핀(STEP/DIR/EN/원점), EN 극성, 센서 극성(Teensy는 3.3 V 신호만), `steps_per_mm`(벨트·풀리·마이크로스텝), 레일 길이, 속도·가속, 위치 이름과 mm(지금 entrance 10 / bed 120 / bathroom 220은 자리값)
2. 배선을 `docs/handoff/reference/electrical-harness-plan.md`에 표로 추가
3. `bash deploy/DOLL-AXIS/flash.sh teensy` (또는 `esp32 COMx`) → `STATUS?`로 센서 확인 → `HOME` → `MOVE` 보정

## PC 시험 결과 (시뮬레이션·빌드만)
가상 캐리지·센서·마이크로초 시계로 native 시험 71/71(새 33개: 원점, 이름 위치 정확한 스텝 수, 최고 속도·가속 준수, 소프트 리밋, 정지·비상정지, 센서 고장, 예상 밖 센서, 파서 20,000 무작위 바이트), CLI 13/13, Teensy 4.1·ESP32 빌드 성공. **보드·모터 실기 시험은 아직.**
