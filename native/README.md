# HomeMind RPI5 네이티브 서비스 (C++)

LG AI Native OS(Yocto scarthgap, glibc 2.39, aarch64)의 RPI5 두 대에서 돌리는 C++ 서비스.
기기에는 Python·Docker가 없다고 보고 만든다(2026-10-22 사용자 방향). `services/`의 Python 코드는
PC 기준 구현이고, 여기 C++ 결과를 같은 입력에서 Python 결과와 대조해 검증한다.

| 노드 | 실행 파일 | 하는 일 |
|---|---|---|
| NODE-RPI5-01 | `homemind-llm` + `llama-server` | `pfn/v1/ai/llm/request` → C++ 가드 → EXAONE(127.0.0.1:8080) → core task request(origin llm, UI 승인) + `llm/result` |
| NODE-RPI5-02 | `homemind-perception` + `whisper-server` | 마이크(ALSA) → 음성 경로(VAD·push-to-talk·whisper·결정론적 파서) + 상황 경로(YAMNet ONNX·헤드·context-fusion) → perception / task request / llm request |
| NODE-RPI5-02 | `homemind-vision` | 카메라(Camera1 D-Bus, PC는 V4L2·파일) → YOLOX ONNX → zone → FIGURE_PRESENCE |
| NODE-RPI5-02 | `homemind-voice-text` | 키보드 문장 → 파서 (시연 대체 경로) |

## 폴더
- `libs/hm_common` 설정·MQTT(libmosquitto)·pfn.v1 메시지·변환표·HTTP
- `libs/hm_text`, `hm_guard`, `hm_parser` 가드·검증·EXAONE 클라이언트·음성 파서
- `libs/hm_audio`, `hm_stt` 캡처·VAD·whisper / `libs/hm_context` YAMNet·헤드·context-fusion
- `libs/hm_vision`, `hm_partial` 비전·부분 인텐트
- `services/*` 실행 파일, `tests/` PC 대조 시험(pytest), `packaging/` IPK, `docker/` 개발 이미지

## PC 빌드·시험 (x86_64 Docker, 시뮬레이션)
```bash
docker build -t homemind-native-dev:24.04 -f native/docker/dev.Dockerfile native/.deps/downloads/onnxruntime-1.20.1
services/runtime/.venv/Scripts/python -m pytest native/tests -q -p no:cacheprovider --basetemp "$TEMP/hmpt/n"
```
`native/tests/hmnative.py`가 Docker 볼륨(`hm-native-build-<builder>`)에 빌드하고, 각 C++ 프로그램의 JSONL probe 출력을
Python 기준 구현 결과·pfn.v1 스키마와 비교한다.

## arm64 빌드와 IPK (QEMU 에뮬레이션)
```bash
docker run --privileged --rm tonistiigi/binfmt --install arm64     # Docker VM 재시작 후 다시 필요
docker buildx build --platform linux/arm64 --load -t homemind-native-dev:24.04-arm64 \
  -f native/docker/dev.Dockerfile native/.deps/downloads/onnxruntime-1.20.1
docker run --rm --platform linux/arm64 -v "$PWD:/w" -v hm-native-build-arm64:/build \
  homemind-native-dev:24.04-arm64 bash /w/native/packaging/build-rpi5.sh
```
결과: `native/dist-ipk/homemind-rpi5-{llm,perception}_<버전>_aarch64.ipk` + `SHA256SUMS`(IPK는 git 제외).
`llama-server`·`whisper-server`는 `native/arm64-tools`에서 교차 빌드한 것(`native/.deps/arm64-tools/bin`).

## 기기 설치 (사용자 승인 후에만)
`deploy/NODE-RPI5-0X/install.sh root@<ip> --broker-host <RPI4-01 주소>` (먼저 `bash deploy/collect.sh all`) — IPK·모델 해시 확인, 모델 복사, `opkg install`,
브로커 주소 설정. MQTT 비밀번호는 기기에서 사람이 `/data/share/homemind/secrets/mqtt-password`로 만든다(스크립트가 방법만 출력).
**QEMU·PC 결과는 Pi 실기 결과가 아니다.**
