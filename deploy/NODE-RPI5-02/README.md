# NODE-RPI5-02 · 인식 (Raspberry Pi 5, LG AI Native OS Video)

USB 마이크·USB 웹캠 2대(방 LifeCam = CAM-INDOOR-01, 현관 = CAM-OUTDOOR-01; Camera Module 3는 Atlas 커널에 CSI 드라이버가 없어 2026-10-25 USB 웹캠으로 바꿈). 원본 음성·영상·전사는 집 밖(외부 서버)으로 나가지 않는다(결과는 MQTT; 집 안 노드끼리는 허용, 사용자 2026-10-25).

| 들어가는 것 | 파일 (`payload/`) | 설치 위치 |
|---|---|---|
| 인식 서비스 | `homemind-rpi5-perception_0.2.1_aarch64.ipk` (25.8 MB, `homemind-clock-sync` 포함: 부팅 20초 뒤·10분마다 core 시각으로 시계 맞춤): `homemind-perception`(ALSA·VAD·누름 통화·whisper 연동·YAMNet ONNX·context-fusion·파서), `homemind-voice-text`(키보드 대체), `homemind-vision`(꺼짐), `whisper-server`/`whisper-cli`, ONNX Runtime·libvosk 등 .so, YAMNet·헤드·부분 인텐트 모델, 계약, 설정 템플릿 `rpi5-02.json`·`whisper.env`, 비전 설정 예시 | `/data/share/homemind/` |
| Whisper small LoRA v4 Q5_0 | `ggml-whisper-small-homemind-lora-v4-q5_0.bin` — IPK 밖 | `/data/share/homemind/models/` |
| (선택) Vosk 한국어 모델 264 MB | `collect.sh NODE-RPI5-02 --with-vosk-model`일 때만 | 부분 인텐트 힌트용, 설정에서 꺼져 있음 |

서비스: `homemind-whisper`(127.0.0.1:8090) → `homemind-perception`. `homemind-vision`은 `config/rpi5-02-vision.json`을 사람이 만들 때까지 꺼져 있다(예시 `share/vision/*.example`).

## 비전 켜기 (1축 인형, 학습 모델 없음)
인형이 레일의 정해진 위치(entrance·bed·bathroom)만 오가므로 **빈 장면과의 차이**로 위치를 판단한다(`detector: background`).
카메라는 이름으로 고른다(방 `LifeCam`, 현관 `USB_Composite_Device`). 서비스 계정 `network`는 유닛의 카메라 D-Bus 그룹으로 Camera1을 쓰고, 받는 폴더는 `/tmp/homemind-vision-indoor|outdoor`(그룹 `camera` 0730, 읽은 사진은 바로 지움).
0. (0.2.3) 설정을 먼저 만들고 `systemctl start homemind-vision`: 빈 장면 전에는 **영상만** 7인치 더보기 → 카메라에 나온다(`preview`, 7인치 Pi5-01에만). 그 화면을 보며 카메라를 고정
1. `share/vision/rpi5-02-vision.json.example` → `config/rpi5-02-vision.json`, `zones.rail.json.example` → `config/zones.rail.json`, `zones.outdoor.json.example` → `config/zones.outdoor.json` (`BROKER_HOST`를 RPI4-01 주소로)
2. 인형을 화면 밖으로 치우고 빈 장면 저장(root): `systemctl stop homemind-vision; LD_LIBRARY_PATH=/data/share/homemind/lib /data/share/homemind/bin/homemind-vision --config /data/share/homemind/config/rpi5-02-vision.json --capture-background`
3. 구역 맞추기: `zones.rail.json`의 첫 띠를 잠깐 화면 전체로 두고, 인형을 각 위치에 둔 채 `... --log-level DEBUG --stdout --camera CAM-INDOOR-01`의 `foot=x,y`(0~1)를 읽어 세 띠를 정한다. 구역 안에 깜빡이는 LED가 있으면 카메라의 `ignore`에 넣는다
4. `systemctl start homemind-vision` → 인형을 옮기며 `journalctl -u homemind-vision`의 `change: zone=` 확인. 카메라·조명 배치를 바꾸면 2번 다시
- 자세한 설명: `native/services/homemind-vision/README.md` "배경 차이 검출기"

## 순서 (사용자 승인 후, RPI4-01 다음)
1. PC: `bash deploy/collect.sh NODE-RPI5-02` (Vosk까지: `--with-vosk-model`)
2. PC: `bash deploy/NODE-RPI5-02/install.sh root@<RPI5-02 주소> --broker-host <RPI4-01 주소> [--broker-port 1884]`
3. 기기에서 MQTT 비밀번호 파일(사용자 `NODE-RPI5-02`) 생성(스크립트가 명령 출력)
4. 마이크 확인: `homemind-perception --list-devices`, 필요하면 `rpi5-02.json`의 `audio_device`

## 누름 통화
- 7인치 UI의 **말하기** 버튼 → `pfn/v1/ai/voice/listen` (core 0.6.1 ACL 필요)
- 기기에서 직접: `echo start > /run/homemind/ptt`

## 기기에서 확인할 것 (P9)
`network` 사용자·`audio` 그룹·ALSA 설정(설치 1단계가 출력), STT 지연(PC 2~4 s/발화), 상황 경로 지연(추정 40~80 ms/창), Vosk 속도·메모리(켜기 전), Camera1 촬영(2026-10-25 확인: 서비스 계정에 카메라 D-Bus 그룹, 받는 폴더 그룹 `camera`), 실제 조명·카메라 노이즈에서 배경 차이 `threshold`, RPI5-01과 시간 동기.

## PC 시험 결과 (시뮬레이션)
Python 기준 구현과 대조 일치, arm64 gtest 통과(QEMU), 설치 상태 시험 `SMOKE_RPI5_PERCEPTION_PASS`(실제 음성 → TV 켜기 요청, 회의 음성 → 조용 모드 ACTIVATED), PC E2E 10/10.
