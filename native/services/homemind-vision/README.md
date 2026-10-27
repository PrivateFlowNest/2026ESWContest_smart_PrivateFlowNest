# homemind-vision — NODE-RPI5-02 인형 재실 서비스

카메라 2대(CAM-INDOOR-01 USB 웹캠, CAM-OUTDOOR-01 USB 웹캠 — 2026-10-25 Camera Module 3에서 바꿈: Atlas 커널에 CSI 카메라 드라이버가 없음) → 검출기 → 박스 하단 중앙점 zone → 안정화 → `pfn/v1/ai/perception` `FIGURE_PRESENCE` (QoS 1, retained 없음). 라이브러리는 `native/libs/hm_vision`(패리티·미검증 항목은 그 README).

검출기(`vision.detector`) 두 가지:
- **`background` (기본 예시, 1축 인형)**: 인형이 레일 위 정해진 구역만 오가므로 학습 모델 없이 **빈 장면과의 차이**로 판단한다. 인형 사진·라벨링·학습이 필요 없고, 현장에서 빈 장면 한 번(`--capture-background`)과 구역 위치만 맞추면 된다. 아래 "배경 차이 검출기" 절
- `yolox`: 학습한 FIGURE 모델(YOLOX-Nano 416, ONNX Runtime). 인형이 자유롭게 움직이는 배치용. 모델은 `services/edge-ai/vision-presence`에서 학습

프레임은 프로세스 밖으로 나가지 않는다. 메시지에는 present·figure_count·zone·confidence·model_version·frame_age_ms만 있다. 좌표는 구역을 맞출 때 쓰는 기기 안 디버그 로그(`--log-level DEBUG`의 `foot=x,y`, 0~1)에만 쓴다.

> Camera1 촬영은 NODE-RPI5-02 실기에서 확인(2026-10-25, USB 웹캠 2대, root와 서비스 계정 `network` + 카메라 그룹). 검출·안정화·발행 루프 전체는 아직 PC x86_64 Docker와 arm64 QEMU에서만.

## 실행

```
homemind-vision --config FILE [--log-level DEBUG|INFO|WARNING|ERROR] [--stdout] [--once]
                [--images PATH [--camera CAM-ID]] [--max-frames N] [--realtime]
homemind-vision --config FILE --capture-background [--frames N] [--camera CAM-ID] [--images PATH]
```

| 옵션 | 동작 |
|---|---|
| (없음) | 설정의 소스로 계속 실행, MQTT 발행. SIGINT/SIGTERM으로 종료 |
| `--stdout` | MQTT 대신 `토픽 JSON` 한 줄씩 출력(Python StdoutPublisher와 같은 형식) |
| `--once` | 카메라마다 한 프레임만 처리하고 안정화 없이 그 결과를 한 번 발행 후 종료(설치 직후 점검용). 카메라를 못 열면 종료 코드 1 |
| `--images PATH` | 모든(또는 `--camera`로 고른) 카메라의 소스를 JPEG/PNG 파일·폴더로 교체. 결과는 `simulated: true` |
| `--max-frames N` | 카메라당 N 프레임 후 종료 |
| `--realtime` | 파일 소스도 `interval_ms` 간격으로 처리(기본은 최대 속도) |
| `--capture-background` | `background` 검출기용: 카메라마다 N장(기본 15, 실카메라는 `interval_ms` 간격)의 픽셀별 중앙값을 빈 장면 PNG(`background_image`)로 저장하고 종료. **인형을 화면 밖으로 치운 뒤** 실행 |

PC 예:

```powershell
# 빌드 이미지 안에서 (repo = /w)
/build/services/homemind-vision/homemind-vision --config /w/native/services/homemind-vision/config/vision.pc.example.json --stdout
```

## 설정 (`config/`)

`hm::RuntimeConfig` 파일(broker·perception_node_id·simulated·contracts_dir) + `vision` 블록. 상대 경로는 `base_dir`(PC: 저장소 루트, 기기: 설정 파일 폴더) 기준. MQTT 비밀번호는 `password_file` 경로만.

| 키 | 기본 | 설명 |
|---|---|---|
| `detector` | `yolox` | `yolox` 또는 `background` |
| `background` | 아래 | `background` 검출기 조정값: `work_width` 160(분석 폭 px), `threshold` 30(밝기 맞춘 뒤 회색 차이), `min_area_ratio` 0.004(분석 영역 대비 최소 덩어리), `max_area_ratio` 0.5(이보다 넓게 바뀌면 판단 불가), `max_gain` 2.0(조명 변화 허용 배율) |
| `model_path` | `yolox`면 필수 | YOLOX raw head ONNX (1×3×416×416 → 1×3549×(5+C)). `background`면 쓰지 않음 |
| `model_version` | `yolox-nano-416-figure` / `background-diff-v1` | perception `attributes.model_version` (1~64자) |
| `threads` | 2 | ORT intra-op 스레드 |
| `score_threshold`, `nms_threshold` | 0.25, 0.45 | detection.py 기본값 |
| `figure_class_ids` | [0] | 인형으로 셀 클래스 (FIGURE 단일 클래스 모델은 0) |
| `interval_ms` | 200 | 카메라당 5 fps (`model-selection.json` target_fps_per_camera) |
| `confirmation_count`, `lost_count` | 3, 3 | ZoneStabilizer |
| `republish_seconds` | 10 | 상태 재발행 주기(0 = 끔). 변화 시·초기 1회는 항상 발행 |
| `presence_fifo` | null | context-fusion용 rpi5-internal `PRESENCE` JSON 줄을 쓸 FIFO(없으면 만듦, 읽는 쪽 없으면 버림). 기기: `/run/homemind/presence` (homemind-perception `presence_fifo`와 같은 경로) |
| `presence_ttl_seconds` | 15 | PRESENCE `valid_until` (> 0) |
| `presence_cameras` | ["CAM-INDOOR-01"] | FIFO에 재실을 쓰는 카메라. fusion은 PRESENCE를 하나(가장 새 것)만 쓰므로 현관 카메라까지 쓰면 방 위치가 10초마다 OUTDOOR/UNKNOWN으로 덮인다 (2026-10-22 agent F, 결정 확인 필요) |
| `cameras[]` | | `camera_id`(CAM-INDOOR-01·CAM-OUTDOOR-01만), `zones`(zone JSON, 파일의 `camera_id`가 같아야 함), `source`, 선택 `interval_ms`, `enabled`. `background` 검출기: `background_image`(기본 `vision-background-<카메라>.png`), `ignore`(비교에서 뺄 정규화 polygon 목록: 상태 LED·화면 등) |

소스 종류:

- `{"type": "camera1", "device_name": "LifeCam", "width": 640, "height": 480, "fps": 30, "format": "JPEG", "warmup_ms": 5000, "capture_dir": "/tmp/homemind-vision-indoor", "capture_dir_mode": "0730", "capture_dir_group": "camera", "request_permissions": true, "app_id": "com.homemind.service.vision"}` — 기기용(2026-10-25 실기 확인)
  - 카메라 고르기: `device_id`(DeviceList 항목 그대로, 예 `camera1`) → 없으면 `device_name`(GetDeviceInfo 이름에 들어 있는 글자, 대소문자 무시, 정확히 한 대만 맞아야 함) → 없으면 `device_index`. `camera1`·`camera2`는 꽂은 순서라 전원을 다시 넣으면 바뀔 수 있어 이름으로 고른다. 실기 이름: `Microsoft®_LifeCam_HD-3000`(방), `USB_Composite_Device`(현관)
  - 받는 폴더: Camera1은 JPEG를 `camera` 사용자로 쓴다. `capture_dir_group: "camera"`면 폴더는 그 그룹 0730, 여기서 만든 상위 폴더는 0710(그 그룹만 넣을 수 있고 아무도 들여다볼 수 없음). 만든 폴더는 끝날 때 비어 있으면 지운다(root로 `--capture-background`를 돌려도 서비스 계정이 쓸 폴더가 남지 않음). 남은 JPEG는 시작할 때 지운다. 카메라마다 `/tmp` 바로 아래 자기 폴더를 쓴다: 두 폴더가 함께 만든 상위 폴더는 먼저 끝난 쪽이 지우지 못해 root 소유로 남을 수 있음
  - 서비스 계정은 D-Bus 그룹 `perm_cameradevice`(Open·Close)·`perm_camerastream`(StartPreview·Capture)·`perm_cameracontrol`(SetFormat)이 있어야 한다(유닛 `SupplementaryGroups`, root는 모두 허용)
- `{"type": "v4l2", "device": "/dev/video0", "width": 640, "height": 480, "fps": 15, "pixel_format": "MJPG"|"YUYV"}` — PC·리눅스 웹캠 대체
- `{"type": "images", "path": "...", "loop": false}` — 시험·오프라인

파일: `vision.pc.example.json`(smoke 모델 + smoke 데이터셋 사진), `vision.v4l2.example.json`, `vision.device.example.json`(기기 기본: `background` 검출기, 실내 카메라 + `zones.rail.json`, 현관 카메라 꺼짐), `vision.device.yolox.example.json`(학습 모델용), `zones.rail.example.json`(1축 레일: ENTRANCE·BED·BATHROOM 띠, 인형 축 펌웨어의 이름 위치 entrance·bed·bathroom과 같은 이름), `zones.indoor.example.json`(Python 템플릿과 같은 임시 polygon), `zones.outdoor.example.json`(전체 화면 OUTDOOR).

## 7인치 실시간 영상 (`vision.preview`, 2026-10-25)
사용자 원칙(2026-10-25): 원본 영상은 집 밖(외부 서버)으로 보내지 않고, 집 안 노드끼리는 공유기 안에서 주고받아도 된다.
`"preview": {"enabled": true, "listen_host": "0.0.0.0", "port": 8091, "allowed_clients": ["10.10.10.3"]}`면 서비스가
카메라마다 **가장 최근 JPEG 한 장만**(Camera1이 준 그대로, 다시 압축 안 함) 메모리에 두고 HTTP로 `allowed_clients`(7인치 Pi5-01)에만
보여 준다(`hm/vision/preview.hpp`). 저장 안 함, MQTT 아님. `GET /preview/<CAMERA-ID>.jpg` 200 / 403 다른 주소 / 404 없음 /
423 사생활 보호 / 503 5초 넘게 새 프레임 없음, `GET /preview/status`(카메라별 `detecting`·프레임 나이). core의
`pfn/v1/system/privacy/state`에서 `camera_allowed` false면 바로 프레임을 버리고 423(7인치 UI도 요청하지 않음).
- 빈 장면이 없는 카메라는 **영상만**(인식·발행 없음, `detecting` false): 카메라를 고정할 때 7인치 화면으로 방향을 맞추고, 구역을 맞출 때도 본다.
  빈 장면을 찍은 뒤 서비스를 다시 시작하면 인식이 켜진다
- Camera1(`com.atlas.Camera1`)은 D-Bus 활성화 서비스라 부팅 뒤 누가 부르기 전까지 꺼져 있다 → 서비스는 `StartServiceByName`으로 깨운다.
  유닛에서 `camera.service`를 끌어오지 않는다: 그러면 `camera-registry.service`(DefaultDependencies=no)가 부팅 5.7초에
  `/var/tmp`가 생기기 전에 돌아 `/tmp/camera_plugin_registry.json`을 못 만들고 Camera1 카메라 목록이 비었다(실기 Pi5-02 재부팅,
  2026-10-25). 그때 고치려면 `systemctl restart camera-registry.service camera.service`

## 배경 차이 검출기 (`detector: background`, 1축 인형)

프레임마다 (`hm/vision/background.hpp`):
1. 프레임과 빈 장면을 분석 폭(160 px)으로 줄이고 회색으로
2. **밝기 맞춤**: 분석 영역에서 픽셀별 비율(프레임 ÷ 빈 장면)의 중앙값으로 나눈다. 바뀌지 않은 픽셀이 대부분이라 방 조명 변화만 반영된다(바닥이 두 밝기로 나뉜 장면에서도 흔들리지 않음)
3. 차이(3×3 평균)가 `threshold` 이상인 픽셀 = 변화. **구역 polygon 안이면서 `ignore` 밖**인 픽셀만 본다
4. 열기·닫기(3×3)로 잡티 제거 → 8방향 연결 덩어리. `min_area_ratio`보다 작은 덩어리는 잡음
5. 조명 배율이 `max_gain` 밖(소등 등)이거나 덩어리 합이 `max_area_ratio`를 넘으면(카메라가 밀림, 손이 들어옴) 그 프레임은 **판단 불가 → 인형 없음**으로 보고, 로그에 한 번 경고한다

덩어리 하나 = 인형 하나. 가장 큰 덩어리의 박스 하단 중앙점으로 구역을 정하고, 이후 안정화·발행은 YOLOX와 같다. 신뢰도는 덩어리 평균 차이가 `threshold`일 때 0.5, 두 배일 때 0.99.

기기 준비 (사람, 카메라를 고정한 뒤):
1. `share/vision/rpi5-02-vision.json.example` → `config/rpi5-02-vision.json`, `zones.rail.json.example` → `config/zones.rail.json`, `zones.outdoor.json.example` → `config/zones.outdoor.json` 복사 후 `BROKER_HOST`를 RPI4-01 주소로(비밀번호 파일·계약 경로는 기기 경로 그대로)
2. **인형을 화면 밖으로 치우고** 서비스를 멈춘 상태에서 빈 장면 저장(설정 폴더에 PNG가 생긴다, root):
   `systemctl stop homemind-vision; LD_LIBRARY_PATH=/data/share/homemind/lib /data/share/homemind/bin/homemind-vision --config /data/share/homemind/config/rpi5-02-vision.json --capture-background`
3. 구역 맞추기: 검출기는 구역 안의 변화만 보므로 처음에는 `zones.rail.json`의 첫 띠를 화면 전체(`[[0,0],[1,0],[1,1],[0,1]]`)로 두고, 인형을 각 정지 위치에 둔 채 `... --log-level DEBUG --stdout --camera CAM-INDOOR-01`로 돌려 `foot=x,y`를 읽는다. 세 위치의 발끝이 각 띠 안에 오도록 띠를 정한다. 구역 안에서 깜빡이는 LED·화면이 있으면 그 자리를 `ignore`에 넣는다
4. `systemctl start homemind-vision` → 인형을 각 위치로 옮기며 `journalctl -u homemind-vision`의 `change: zone=` 확인
- 카메라 위치·각도나 조명 배치를 바꾸면 3번을 다시 한다. 빈 장면 PNG는 기기 밖으로 보내지 않는다

기동 시 검사: 카메라가 `device-registry.json`에서 `perception_node_id`(NODE-RPI5-02)에 연결돼 있는지, zone 파일의 camera_id가 맞는지. 카메라를 못 열면 1~30초 백오프로 다시 연다(파일 소스는 바로 오류).

## 시험 결과 (PC x86_64 Docker)

`native/tests/test_vision_parity.py`의 서비스 항목:
- `--stdout` 전체 실행: 카메라마다 초기 상태 1건(smoke 모델이라 present=false, zone unknown), perception 스키마·registry 통과
- `--once`: 두 카메라 각 1건
- MQTT: 컨테이너 안 Mosquitto로 발행 → 구독 2건 수신, 스키마 통과
- zone: COCO 후보 모델 + `figure_class_ids [16,1,2]`로 dog.jpg → zone·개수·신뢰도가 Python `ZoneMap.locate` 계산과 같음

## 남은 일 / 확인 필요

- 기기에서 Camera1: DeviceList ID ↔ 카메라 대응(시작 로그에 DeviceList·GetDeviceInfo 출력), Capture 속도, capture_dir 권한, 서비스 권한 선언(serviceinfo.json)
- 한 스레드가 카메라를 번갈아 처리한다. Camera1 재연결(5초 warmup) 동안 다른 카메라도 멈춘다
- 실제 FIGURE 모델·실제 방 polygon 없음(템플릿)
- context-fusion으로 재실을 넘기는 방식: 로컬 FIFO로 연결함(2026-10-22, homemind-perception `presence_fifo`). PC 시험(x86_64 Docker):
  COCO 후보 모델 + dog.jpg(실시간 5 fps) → FIFO → homemind-perception(AIHub 회의 24초, 실시간) → ACTIVITY_CONTEXT 21창 전부
  zone `desk`(vision의 zone), 현관 카메라 재실은 FIFO에 쓰이지 않음, 같은 입력을 Python FeatureStreamFusion에 넣은 결과와 21/21 동일
  (`native/tests/test_perception_live.py`). systemd: 두 유닛 모두 `RuntimeDirectory=homemind` + `RuntimeDirectoryPreserve=yes` 필요(패키징)
