# hm_guard / hm_text / hm_parser / homemind-llm / homemind-voice-text (C++ 포팅, 에이전트 A)

RPI5(LG AI Native OS, Python·Docker 없음)용 네이티브 C++20 구현. Python 원본이 기준(oracle)이고,
**결과는 모두 PC x86_64 Docker(및 QEMU arm64 에뮬레이션)에서 얻은 것이다. 실기(라즈베리파이) 결과가 아니다.**

## 무엇을 포팅했나

| C++ | Python 원본 |
|---|---|
| `libs/hm_text` (`hm/text.hpp`, `hm/pyjson.hpp`, `hm/pyregex.hpp`) | Python `str`/`json`/`re` 의미: `casefold`(utf8proc, 전체 case folding C+F), `strip`/`isspace`, `isprintable`/`repr`, `len`(코드 포인트), `float.__repr__`, `round()`, `json.loads`(CPython 3.12 C 스캐너, 같은 오류 메시지), `json.dumps(ensure_ascii=False)`, `==`/진리값, dict 삽입 순서(`nlohmann::ordered_json`), 64비트를 넘는 정수(정확한 int로 유지), PCRE2(UTF+UCP)에서 `\s`·`\S`·`\d`를 Python 집합으로 치환 |
| `libs/hm_guard` `guardrails` | `homemind_exaone/guardrails.py` `preflight_proposal` (15ec030 반영: 미지원 가전 14종, 검사 우회 문구, 응급·침입) |
| `libs/hm_guard` `validation` | `validation.py` `validate_proposal`, `validate_activity_context`, `safe_fallback`, 오류 문자열·순서 동일 (재시도 프롬프트에 그대로 들어감) |
| `libs/hm_guard` `exaone_client` | `client.py` `ExaoneClient`: SYSTEM_PROMPT, few-shot, `_payload`(요청 바디 바이트 단위 동일), `/v1/chat/completions` POST, 응답 파싱, 재시도 피드백, 결정론 후보 병합, fallback, origin |
| `libs/hm_guard` `pfn_schema` | `pfn.validate`(jsonschema)가 하는 llm-request / task-request(+device-control) / llm-result 검사와 같은 수용·거부 (`format: uuid`의 jsonschema 동작 포함) |
| `libs/hm_parser` `VoiceParser` | `homemind_runtime/voice.py` `VoiceParser` (`voice_parser.hpp` 인터페이스) |
| `services/homemind-llm` | `llm_node.py` `LlmNode` + `cli.run_llm` |
| `services/homemind-voice-text` | `cli.run_voice_text` + `ContextTracker` |

## 실행 파일

```
homemind-llm --config runtime.json [--log-level INFO] [--once-stdin]
homemind-voice-text --config runtime.json [--log-level INFO] [--dry-run]
```
- 설정 키는 `services/runtime/config/runtime.pc.example.json`과 같다(broker, llm_base_url, llm_model, llm_node_id,
  perception_node_id, llm_request_ttl_ms, simulated, contracts_dir). MQTT 비밀번호는 `broker.password_file` 경로로만 읽는다.
- 추가 키 `capabilities_path`(선택): `device-capabilities.json` 위치(상대 경로는 base_dir 기준). 없으면
  `$HM_CAPABILITIES_PATH` → 빌드 트리의 `services/exaone-edge/config/device-capabilities.json` → `$HM_BASE_DIR/...` 순.
- `--once-stdin`: llm/request JSON 한 줄당 `{"messages": [[topic, doc], ...]}` 또는 `{"error": ...}` 한 줄 (테스트용).
- `--dry-run`: MQTT 없이 발행할 메시지를 `PUBLISH <topic> <json>`으로 출력.
- 타임아웃은 Python `run_llm`과 같이 30초, max_tokens 256, retries 1.
- 전사문(transcript)은 로그·결과 어디에도 쓰지 않는다. 거부 사유 메시지에도 값은 넣지 않는다.

런타임 의존성(ldd, x86_64 Release): `libmosquitto.so.1`(→ `libssl.so.3`, `libcrypto.so.3`), `libstdc++.so.6`, `libm.so.6`,
`libgcc_s.so.1`, `libc.so.6`. utf8proc와 PCRE2는 정적 링크(`HM_TEXT_STATIC_DEPS=ON`, 기본값)라 기기에 별도 패키지가 필요 없다.

## Parity 검증 방법

`native/tests/` (Windows venv의 Python 원본과 Docker 안 C++ 프로브를 같은 입력으로 비교):

```
set HM_NATIVE_BUILDER=guard
services/runtime/.venv/Scripts/python -m pytest native/tests/test_guard_parity.py native/tests/test_parser_parity.py native/tests/test_llm_node.py -p no:cacheprovider --basetemp %TEMP%/hmpt/a1 -s
```
- 빌드는 `guard_support.build_own()`: hm_common과 이 폴더들만 담은 심볼릭 링크 트리(/tmp/ws)를 configure해서
  다른 에이전트의 미완성 CMakeLists가 테스트를 깨지 않게 한다(공유 트리 `hmnative.build()`로도 빌드됨을 확인).
- 가짜 llama-server(`guard_support.FakeLlama`, Python http.server)를 호스트에 띄우고 컨테이너는 `host.docker.internal`로 접속.
  Python 클라이언트와 C++ 클라이언트가 같은 스크립트 응답을 받고, 서버가 받은 요청 바디를 바이트 단위로 비교한다.
- MQTT 종단 테스트는 컨테이너 안에서 mosquitto + homemind-llm / homemind-voice-text를 띄워 확인한다.
- 각 테스트는 결과 수치를 `%TEMP%/hm_guard_parity.json`, `hm_parser_parity.json`, `hm_llm_node.json`에 남긴다.

## 결과 (2026-10-22, PC x86_64 Docker, Ubuntu 24.04 / GCC 13, Python 3.12.10)

| 항목 | 동일/전체 |
|---|---|
| preflight: acceptance-cases-v1 (100문장 × 컨텍스트 9~10종) | 904/904 |
| preflight: acceptance-independent-v1 / v2 | 723/723, 724/724 |
| preflight: smoke-cases / pc-e2e-cases-v1 | 216/216, 90/90 |
| preflight: finetune-v1 / v2 / v3 (train+validation+test, 자기 컨텍스트 포함) | 27564/27564, 27564/27564, 36474/36474 |
| preflight: Python 테스트 파일의 문장 96개(112개 테스트 포함) | 864/864 |
| preflight: 합성 변형 6024문장(대소문자, 전각/아랍 숫자, 유니코드 공백, 주입·부정·위험 접두사 등) | 54216/54216 |
| **preflight 합계** | **149339/149339** |
| preflight: capabilities 변형 7종 | 2961/2961 |
| validate_proposal (정상 68, 오류 3437, TypeError 2) | 3507/3507 (오류 목록·순서 포함) |
| validate_activity_context | 537/537 |
| ExaoneClient 요청 바디(무작위 400건: 피드백·후보·컨텍스트·모델명·few-shot on/off) | 400/400 바이트 동일 |
| finetune-v3 학습 프롬프트와 C++ 프롬프트 messages 일치 | 3831/3831 (Python도 3831/3831) |
| interpret + 가짜 llama-server (정상/펜스/쓰레기/잘못된 enum/추가 키/미지원 기기/NaN/큰 정수/BOM/HTTP 4xx·5xx/연결 끊김/타임아웃/연결 거부) | 290/290, 재시도 포함 요청 바디 522개 바이트 동일 |
| VoiceParser: 전체 코퍼스 10909문장 × (컨텍스트 없음, 회의) | 21818/21818 (C++ 메시지 7246개 계약 스키마 통과) |
| VoiceParser: 인자 변형 4000건(stt_confidence·모델명·id·노드 id·ttl·simulated, 예외 323건 포함) | 4000/4000 |
| homemind-voice-text --dry-run / MQTT + perception 컨텍스트 | 출력 줄·메시지 동일 |
| llm-request 스키마 검사(변형 654건) / jsonschema uuid 형식(22건) | 654/654, 22/22 |
| LlmNode (`homemind-llm --once-stdin`) vs Python LlmNode | 84/84, llama 요청 바디 78개 동일 |
| MQTT 종단: expired → expired 결과, 모델 출력 → task request(origin llm) + task_requested, guard → no_action, 모델 다운 → model_unavailable, 스키마 위반 2건 → 발행 없음 | 통과, 로그·결과에 전사문 없음 |
| 기반 함수: casefold(모든 코드 포인트, 1530개 변환) / isspace / decimal | 전부 동일 |
| isprintable | Unicode 15.1에서 새로 배정된 627자(U+2FFC–2FFF, U+31EF, CJK 확장 I)만 다름 (Python 3.12 = Unicode 15.0) |
| repr/strip/len, json.loads 80종, float repr 5018개, UTF-8 디코드 오류 3014개, guard 정규식 17개 × 334 문장 | 전부 동일 |

gtest: hm_text 9, hm_guard 48(Python test_guardrails/test_validation/test_client 이식 + 세부), hm_parser 6,
homemind-llm 4, homemind-voice-text 2 — 모두 통과. pytest(위 3개 파일) 24개 통과.

arm64(`homemind-native-dev:24.04-arm64`, PC의 QEMU 에뮬레이션 — 실기 아님): 전체 타깃 Release/Debug 빌드 성공(-Werror 경고 없음),
hm_text·hm_guard gtest 통과, preflight 149339/149339 동일. 그 뒤 Docker의 arm64 에뮬레이션(binfmt)이 사라져
(`exec format error`) 나머지 parity는 arm64로 돌리지 못했다. binfmt 재등록은 시스템 설정 변경이라 하지 않았다.

## 의도적·알려진 차이 (모두 발행 메시지에는 영향 없음)

- 전송 오류 문구: Python은 `<urlopen error [WinError 10061] ...>`, `timed out`, `Remote end closed connection ...` 등
  OS별 문구, C++은 `<urlopen error connect ...: Connection refused>`, `timed out`. 결과(origin, fallback, attempts)는 같고
  문구는 `InterpretationResult.errors`에만 남는다(발행 안 함, 재시도 피드백에도 안 들어감).
- HTTP 오류 문구는 표준 reason phrase 표를 쓴다(`HTTP Error 503: Service Unavailable`). llama-server/http.server와 같다.
- urllib의 3xx 리다이렉트 추적, `api_key`(Authorization 헤더)는 포팅하지 않았다(llama-server는 127.0.0.1, run_llm도 키 없음).
- 타임아웃: Python은 소켓 연산마다 30초, C++(hm::http)은 요청 전체 30초.
- 매우 깊은 JSON(1000단계 초과)은 둘 다 RecursionError지만 정확한 한계 깊이는 다를 수 있다.
- homemind-voice-text는 Python이 예외로 종료하는 입력 줄(스키마 위반 등)을 로그에 남기고 다음 줄로 넘어간다.
- 스키마 위반 사유 문자열은 jsonschema 문구가 아닌 자체 문구(수용·거부 판단은 동일).

## Python 원본에서 발견한 문제 (parity 때문에 C++도 같은 동작, 수정은 리드 판단)

1. **전사문 로그 유출**: Python LlmNode는 llm/request가 스키마를 어기면(예: 500자 초과 transcript) jsonschema 메시지에
   전사문이 그대로 들어간 ValueError를 내고 Bus가 `log.exception`으로 남긴다. C++은 값 없이 사유만 남긴다.
2. guard가 capabilities에 없는 쌍을 만든다: "세탁기 켜줘" → WASHER/TURN_ON → 검증 실패 → fallback(MODEL_OUTPUT_INVALID).
3. 볼륨·밝기 정규식 `[^0-9]{0,12}(\d{1,3})`: `[^0-9]`가 비ASCII 숫자도 먹어서 "TV 볼륨 ２０으로" → volume 0, "٣٥" → 5.
4. hm_common `round_to`/`confidence4`(nearbyint(x*10^n)/10^n)는 Python `round()`와 다르다: 0.12345 → 0.1234(Python 0.1235),
   0.00005 → 0.0(0.0001), 0.33335 → 0.3334(0.3333). hm_parser/homemind-llm은 Python과 같은 `py::round_digits`로 먼저
   반올림해서 결과가 같다. perception 쪽(confidence4 직접 사용)은 영향받을 수 있다.
