# NODE-RPI5-01 · 7인치 UI + EXAONE (Raspberry Pi 5, LG AI Native OS Video)

| 들어가는 것 | 파일 (`payload/`) | 설치 위치 |
|---|---|---|
| LLM 노드 | `homemind-rpi5-llm_0.2.0_aarch64.ipk` (8.7 MB): `homemind-llm`(C++ 가드 + EXAONE 클라이언트), `homemind-clock-sync`(부팅 20초 뒤·10분마다 core 시각으로 시계 맞춤), `llama-server`, 동봉 .so, 계약, 설정 템플릿 `rpi5-01.json`·`exaone.env` | `/data/share/homemind/` |
| 7인치 UI | `homemind-rpi5-ui_0.3.5_aarch64.ipk` (18.4 MB): Flutter Wayland 번들(오픈소스 flutter-elinux, RUNPATH = Atlas 앱 폴더), Atlas 앱 `appinfo.json`, 감시 스크립트 `homemind-ui-atlas.sh`, `homemind-ui.service`, `ui.env` | `/data/share/homemind/ui/` → Atlas 앱 `/data/share/usr/atlas/apps/com.atlas.app.homemind_ui/` |
| EXAONE v3 모델 | `exaone-1.2b-homemind-v3-q4_k_m.gguf` (812 MB) — IPK 밖 | `/data/share/homemind/models/exaone-1.2b-homemind-q4_k_m.gguf` |

서비스: `homemind-exaone`(llama-server, 127.0.0.1:8080) → `homemind-llm`(MQTT), `homemind-ui`(Atlas 앱 감시).

### 7인치 UI가 Atlas 앱으로 도는 이유 (실기 2026-10-23)
Atlas 화면 관리자(`window-manager`, `/run/xdg/wayland-0`)는 **Atlas 앱 관리자가 실행한 앱에만** 화면 층을 준다. systemd가 띄운 창은 접속은 되지만 안 보인다(`Failed to find layer`). 그래서 `homemind-ui.service`(root)가
1. 번들을 Atlas 앱 폴더로 복사(바뀐 때만, 소유자 = 앱 계정)
2. `config/ui.env`의 키를 앱의 `ui.env`(`/data/profiles/0/appdata/com.atlas.app.homemind_ui/home/`)로, MQTT 비밀번호 파일은 앱 계정만 읽는 0400 사본으로
3. `com.atlas.AppManager1.Start`로 실행하고, 꺼지면 10초 안에 다시 실행한다. 서비스를 멈추면 앱도 멈춘다

설정을 바꾸면 `systemctl restart homemind-ui`. Atlas는 앱을 보안 실행 모드로 띄워 `$ORIGIN`·`LD_LIBRARY_PATH`를 무시하므로 번들 RUNPATH는 앱 폴더 절대 경로다.
**전제**: 앱 `com.atlas.app.homemind_ui`가 Atlas 패키지 관리자에 등록돼 있어야 한다(지금 Pi5-01에는 있음). 초기화한 기기에서 등록하는 방법(`com.atlas.PackageManager1.Install`)은 아직 확인 전.

## 순서 (사용자 승인 후, RPI4-01 다음)
1. PC: `bash deploy/collect.sh NODE-RPI5-01`
2. PC: `bash deploy/NODE-RPI5-01/install.sh root@<RPI5-01 주소> --broker-host <RPI4-01 주소> [--broker-port 1884]`
   - payload 해시 확인 → 기기 상태 출력 → 모델 복사(해시 확인) → IPK 2개 설치 → `rpi5-01.json`·`ui.env`의 브로커 주소 자리값 교체 → 비밀번호 파일 확인 → 서비스 상태
3. 기기에서 MQTT 비밀번호 파일(사용자 `NODE-RPI5-01`)을 만든다. 스크립트가 명령을 출력한다(값은 화면에 안 보임)

## 기기에서 확인할 것 (P9)
- 7인치 화면에 HomeMind가 뜨고 브로커에 로그인하는지(`journalctl -u homemind-ui`, 앱 상태 `busctl call com.atlas.AppManager1 /com/atlas/AppManager1 com.atlas.AppManager1 GetStatus s com.atlas.app.homemind_ui`)
- RPI5-02와 시간 동기(NTP). 말하기 버튼 요청은 3초 지나면 버려진다
- EXAONE 응답 시간(PC GPU 0.4~0.5 s, Pi5 CPU는 측정 필요)
- llama.cpp 빌드를 바꾸지 않는다(시스템 프롬프트 처리 문제로 결과가 달라질 수 있음, system-architecture 3-2)

## PC 시험 결과 (시뮬레이션)
arm64 gtest 통과(QEMU), 설치 상태 시험 `SMOKE_RPI5_LLM_PASS`(v3로 task_requested, 전사 로그 없음), UI 번들 headless weston 1024×600 + 인증 브로커 로그인 확인, PC E2E 10/10.
