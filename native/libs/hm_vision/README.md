# hm_vision — NODE-RPI5-02 비전 재실(FIGURE_PRESENCE) C++ 라이브러리

`services/edge-ai/vision-presence`(Python 기준 구현)의 C++ 포팅. 서비스 실행 파일은 `native/services/homemind-vision`.

> 모든 결과는 **PC x86_64 Docker(시뮬레이션)** 와 arm64 QEMU 에뮬레이션이다. Raspberry Pi 실기에서 돌린 적 없다.

## 포팅 범위

| Python | C++ | 비고 |
|---|---|---|
| `cv2.imread(IMREAD_COLOR)` | `read_image`, `decode_jpeg`, `decode_png` (`image.hpp`) | libjpeg-turbo(ISLOW·fancy upsampling, BGR 출력), EXIF orientation 1~8 적용, libpng 변환 순서 OpenCV와 동일. CMYK JPEG는 거절 |
| `cv2.resize(INTER_LINEAR)` | `resize_linear` | OpenCV 고정소수점(11bit 계수, `>>4 … >>16 … +2>>2` 수직 보간)과 정확히 2배 축소 시 INTER_AREA 경로까지 재현 |
| `cv2.cvtColor(COLOR_YUV2BGR_YUYV)` | `yuyv_to_bgr` | V4L2 YUYV용 (OpenCV cap_v4l과 같은 변환) |
| `detection.preprocess_bgr` | `preprocess_bgr` | 416 레터박스, 패딩 114, BGR, 0~255, NCHW float32 |
| `detection.decode_raw_predictions`·`postprocess`·`_nms`·`_iou` | `postprocess` | float32 연산 순서 그대로, 클래스별 NMS, 신뢰도 내림차순 안정 정렬 |
| `detect_file`의 ORT 세션 | `make_onnxruntime_backend` (`InferenceBackend`) | intra_op=threads, inter_op=1. Inference Gateway(`ecoai-inference`)는 같은 인터페이스 뒤에 추가할 수 있게 분리 |
| `zones.BoundingBox`·`ZoneMap`·`ZoneStabilizer`·`_point_in_polygon` | `zones.hpp` | 오류 메시지·검사 순서·**설정 파일의 polygon 순서**(ordered_json) 동일 |
| (없음) | `presence.hpp` | 아래 "네이티브 전용 판단" |
| `cv2.VideoCapture` | `sources.hpp`: images(파일·폴더), V4L2(MJPG/YUYV mmap), **Camera1 D-Bus** | |
| `dataset._proposed_bbox`(라벨링 보조, 같은 생각) | `background.hpp` `BackgroundDetector`, `median_image`, `write_png` | **네이티브 전용**: 1축 인형용 모델 없는 검출기(빈 장면과의 차이, 픽셀별 비율 중앙값으로 밝기 맞춤, 구역 안·`ignore` 밖만, 판단 불가 프레임 거름). Python 대조 대상 없음 → gtest 8개 + 서비스 시험 `native/tests/test_vision_background.py` |

`-ffp-contract=off -fno-fast-math`: aarch64 GCC가 기본으로 a*b+c를 FMA로 합치면 numpy float32 결과와 달라지므로 끈다.

## 패리티 (PC x86_64 Docker, `native/tests/test_vision_parity.py`)

Python 쪽: vision-presence venv (cv2 5.0.0, numpy 2.5.4, onnxruntime 1.29.0, Windows). C++ 쪽: Ubuntu 24.04, ONNX Runtime 1.20.1.

| 항목 | 입력 | 결과 |
|---|---|---|
| 디코딩 | 샘플 42장(data·artifacts의 jpg/png) + 변형 19장(gray/progressive/4:4:4/4:2:2 JPEG, EXIF 1~8, PNG rgb/gray/bgra/16bit/palette/1bit) | **61/61 픽셀 완전 일치** |
| resize | 38건(카메라 해상도, 2배 축소, 확대, 1×1, 임의 크기) | **38/38 바이트 일치** |
| YUYV | 5건 | **5/5 일치** |
| 전처리 텐서 | 42장 | **42/42 float32 완전 일치** |
| 검출 smoke 모델 | 42장 × 0.25 | 검출 0 = Python 0, 원시 출력 최대 차 0.0, best score 차 0.0 |
| 검출 COCO 후보(yolox_nano_416, 80클래스) | 42장 × (0.25, 0.05, 0.01) = 126건, NMS 생존 142개, 후보 835개 | 생존 집합·순서·클래스 동일, 박스 최대 차 **6.1e-5 px**, 점수 차 **0.0**, 원시 출력 차 0.0 |
| 같은 raw로 후처리 | COCO raw 18건 + 합성 raw 39건(1·3클래스, 겹치는 박스, NMS 0.3/0.45/0.7) | NMS 생존 21,882개 동일, 박스 최대 차 2.4e-4 px(np.exp vs expf 1ulp), 점수 차 0.0 |
| zone | 설정 13개(템플릿 + 오목 다각형 무작위) × 504박스(경계선 위 발끝점 포함, 잘못된 박스 포함) = 6,552 | **6,552/6,552 동일**(오류 메시지 포함) |
| zone 설정 오류 | 9건 | 메시지·허용 여부 동일 |
| ZoneStabilizer | (확정,유실) 5조합 × 10열, 15,060 update | 동일 |
| 메시지 | 빌더 14건, 서비스 `--stdout`·`--once`·MQTT(컨테이너 내 Mosquitto) | perception.schema.json·registry(zone, 장치-노드) 통과, rpi5-internal PRESENCE 스키마 통과 |
| 서비스 zone | COCO 모델 + `figure_class_ids=[16,1,2]`로 dog.jpg | 발행 zone(`bathroom`)·개수(3)·신뢰도 = Python `ZoneMap.locate` 결과 |

허용 기준(박스 1px, 점수 1e-4, NMS 생존 동일, zone 동일)을 모두 크게 밑돈다. 점수 동점: 실제 검출 0건, 합성 1건(생존 결과에 영향 없음).

arm64(QEMU, `homemind-native-dev:24.04-arm64`) 결과는 아래 "arm64" 절.

## arm64 (QEMU 에뮬레이션, 실기 아님)

`HM_NATIVE_IMAGE=homemind-native-dev:24.04-arm64`로 같은 시험(지연·서비스 제외 13개) 통과. `-Werror` 빌드, gtest 21/21.

| 항목 | aarch64 결과 |
|---|---|
| 디코딩 61 · resize 38 · YUYV 5 · 전처리 텐서 42 | 전부 비트 일치 (FMA 끔) |
| smoke 모델 | 검출 0 = Python, 원시 출력 최대 차 6.0e-8 |
| COCO 후보 126건 | NMS 생존 142개 동일, 박스 최대 차 5.2e-4 px, 점수 차 1.9e-6, 원시 출력 차 3.2e-4 (ORT aarch64 커널 차이) |
| 같은 raw 후처리 57건 · zone 6,552 · 안정화 15,060 | x86과 같은 결과 |

## 지연 (PC, 참고용)

`test_latency_pc` (Release 빌드, 200회, Ryzen 9 5900X, **다른 에이전트 빌드가 동시에 돌던 부하 상태**). 전처리+추론+후처리 합계 P50:

| 모델 | 이미지 | threads | C++ (Docker) | Python (Windows) |
|---|---|---|---|---|
| smoke | 768×576 | 4 | 11.1 ms (P95 17.5) | 14.1 ms |
| smoke | 768×576 | 2 | 17.0 ms | 21.2 ms |
| COCO nano | 768×576 | 4 | 24.2 ms | 19.9 ms |
| COCO nano | 320×240 | 2 | 15.4 ms | 16.0 ms |

C++ 전처리(디코딩 제외)는 0.7~1.4 ms. 추론 시간은 ORT가 지배하고 PC 부하에 따라 흔들린다. **Pi 5 값이 아니다.**

## 네이티브 전용 판단 (Python에 없는 부분)

Python 기준 구현은 박스·zone까지만 있고 실시간 루프가 없다. 다음은 C++에서 정한 것으로 README·리드 보고에 표시한다.

- 박스 정규화: 픽셀 박스 ÷ (이미지 폭, 높이). 클립 후 폭·높이가 0인 박스는 개수에만 넣고 zone 판정에서 뺀다
- 대표 인형: figure 클래스(`figure_class_ids`, 기본 [0]) 중 신뢰도가 가장 높고 판정 가능한 박스의 발끝점
- 발행: 안정 zone이 바뀔 때(`ZoneStabilizer` 3프레임 확정·3프레임 유실), 처음 3프레임 뒤 초기 상태 1회, 이후 `republish_seconds`(기본 10초)마다 (retained 없음이라 UI 재시작 대비)
- confidence: 인형이 있으면 대표 박스 점수, 없으면 `1 − (전체 제안 중 최고 점수)`
- `present` = 현재 프레임 인형 수 > 0, `zone` = 안정 zone(registry 소문자로 `ai-action-map.json` zone_map 변환), `frame_age_ms` = 발행 시각 − 촬영 시각
- 메시지에는 contract가 허용한 필드만(좌표·박스·이미지 없음). `simulated`는 설정 `simulated` 또는 파일 소스면 true
- context-fusion용 rpi5-internal `PRESENCE` 관측(`presence_observation`)을 로컬 FIFO로 낼 수 있음(서비스 `presence_fifo`, 기본 꺼짐)

## 미검증

- Camera1 D-Bus 소스(`camera1_source.cpp`): **2026-10-25 NODE-RPI5-02에서 촬영 확인**(USB 웹캠 2대, root와 `network` + `perm_cameradevice`·`perm_camerastream`·`perm_cameracontrol` 그룹, 받는 폴더 그룹 `camera` 0730, 카메라마다 `/tmp/homemind-vision-indoor|outdoor`). 그 전 기록: 공식 샘플(`docs/ai-native-os-sample/cameraapp`)의 호출 순서(PermissionAgent1 user_consent 확인·부여 → DeviceList → Open → SetFormat(width/height/fps `u`, format `s` JPEG) → StartPreview(handle, 0) → 5초 대기 → 프레임마다 Capture(handle, 1, dir))를 sd-bus로 옮겼다. PC에서는 컴파일과 "시스템 버스 없음 → 예외" 경로만 확인. 확인할 것: DeviceList ID와 두 카메라 대응, Capture 속도(5 fps 가능한지), capture_dir 권한(서비스가 쓸 수 있는지), 서비스(앱이 아님)의 권한 선언 방식, FaultEvent/DeviceRemoved 신호 이름·인자
- Capture는 JPEG를 파일로 쓴다(샘플이 보여준 유일한 방식). 읽자마자 지우고, 시작 시 전용 폴더의 남은 jpg를 지운다. 공유 메모리(GetFd/GetBufferInfo) 경로는 형식 자료가 없어 구현하지 않음
- V4L2 소스: 실제 웹캠으로 돌려 보지 않음(Docker에 /dev/video 없음). YUYV 변환과 MJPEG 디코딩만 cv2와 대조
- 실제 FIGURE 모델이 아직 없다. smoke 모델은 모든 위치에서 점수 ≈1e-4로 검출이 없으므로 NMS·박스 패리티는 COCO 후보 모델로 증명했다
- PNG eXIf 방향 태그는 적용하지 않음(OpenCV 최신판은 적용할 수 있음). CMYK JPEG 거절

## 의존성 (모두 선택, 없으면 해당 기능만 실행 시 예외 · gtest는 skip)

| 기능 | 빌드 패키지 (Ubuntu 24.04) | 실행 .so |
|---|---|---|
| 추론 | ONNX Runtime 1.20.1 (`HM_ONNXRUNTIME_ROOT`) | libonnxruntime.so.1 |
| JPEG·MJPEG·Camera1 프레임 | `libjpeg-turbo8-dev` | libjpeg.so.8 |
| PNG | `libpng-dev` | libpng16.so.16 |
| Camera1 | `libsystemd-dev` (sd-bus) | libsystemd.so.0 (기기 것을 쓰고 동봉하지 않는 것을 권장) |

빌드 이미지: `native/docker/dev.Dockerfile`(위 3개 패키지 포함, 2026-10-22 통합). 패키지가 없는 이미지에서도 경고 없이 빌드됨을 확인.

## 시험

```powershell
$env:HM_NATIVE_IMAGE = "homemind-native-dev:24.04"; $env:HM_NATIVE_BUILDER = "vision"
services\edge-ai\vision-presence\.venv\Scripts\python -m pytest native\tests\test_vision_parity.py -s -p no:cacheprovider --basetemp "$env:TEMP\hmpt\d1"
```

gtest `hm_vision_tests` 21개(리사이즈·YUYV·EXIF·후처리·NMS·zone·안정화·발행 정책·메시지·소스·smoke 모델)는 위 시험의 첫 항목으로 함께 돈다. `tools/hm_vision_probe`는 시험 전용 JSONL 도구(픽셀·텐서를 파일로 쓴다). 서비스는 그런 출력을 하지 않는다.
