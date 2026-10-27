# hm_partial — 부분 발화 인텐트(partial intent) C++ 라이브러리

`services/edge-ai/partial-intent`(Python 기준 구현)의 C++ 포팅. STT partial 문장 → 준비 힌트. **실행 권한 없음**(`execution_allowed`는 항상 false). 음성 서비스(다른 에이전트)가 링크해서 쓰는 라이브러리이며, 이 폴더에는 시험용 probe만 있다.

> 모든 결과는 **PC x86_64 Docker(시뮬레이션)** 와 arm64 QEMU 에뮬레이션이다. Raspberry Pi 실기 아님.

## 포팅 범위

| Python | C++ | 비고 |
|---|---|---|
| `features.normalize_text` | `text.hpp normalize_text` | NFKC(utf8proc) → `str.strip()` → `str.lower()`(U+0130 → i̇, 그리스어 final sigma 포함) → `\s+` → " " → 앞 512 코드포인트. 공백 집합은 Python `str.isspace()`와 같음 |
| (UTF-8 입력) | `decode_utf8` | 잘못된 바이트는 Python `surrogateescape`처럼 U+DC80~DCFF로 → `predict`가 malformed로 보고 NO_ACTION/SUPPRESSED, confidence 1.0 (Python과 같은 경로) |
| `fnv1a_32`, `vectorize_one` | `intent.hpp vectorize_one` | 1~5 문자 n-gram, `"{n}:{token}"` UTF-8(replace) 해시, 부호 비트, float32 누적, 의미 단서 25칸, L2 정규화 |
| `policy.apply_safety_policy`, `requires_final_transcript` | 같은 이름 | 문구 목록은 Python 모듈에서 생성, 시험이 매번 대조 |
| `taxonomy.HINTS` | `hints_for` | |
| `IntentRuntime.predict` | `IntentRuntime::predict` | ORT intra/inter 1, 이벤트 필드 동일(`text_sha256`, `text_length`, `round(confidence, 6)`, ISO 시각 `+00:00`) |
| `StreamIntentGate` | `StreamIntentGate` | 같은 의도 DETECTED 2회(부분 문장) 확인, 이미 낸 의도 억제, threshold = metadata `streaming_threshold`(0.3) |
| (없음) | `UtteranceIntentStream` | utterance_id가 바뀌면 게이트 새로 만듦(아래 "Python 버그") |
| (없음) | `hint_perception` | DETECTED 이벤트 → pfn.v1 perception `PARTIAL_INTENT_HINT` |

원문은 이벤트·perception 어디에도 없다(해시·길이만). 로그도 남기지 않는다.

## 패리티 (PC x86_64 Docker, `native/tests/test_partial_parity.py`)

Python 쪽: partial-intent venv(numpy 2.5.2, onnxruntime 1.29.0). 입력: `data/**/*.jsonl` 전부 5,321문장(v1·v2 train/validation/test, acceptance v1~v3) + 경계 사례 25개(빈 문장, 전각, NUL, 이모지, 결합 문자, 호환 자모, 600자, 제어 공백 등) + 잘못된 UTF-8 5개.

| 항목 | 결과 |
|---|---|
| 문구·HINTS 표 | 완전 일치 |
| 정규화(문자열·길이·SHA-256) | **5,351/5,351 일치** |
| 특징 벡터 | 0이 아닌 위치 전부 일치(507,115개), 값 최대 차 **1.19e-7**(1ulp), 4,192/5,346 벡터는 비트 일치. 차이는 L2 norm 합산 순서(numpy는 BLAS sdot, C++은 double 누적)뿐 |
| 확률(ONNX) | 최대 차 **2.4e-7**, argmax 5,346/5,346 일치 |
| predict 이벤트 (is_final 둘 다, 10,702건) | intent·result·후보·준비·해시·길이·is_final·model **전부 일치**, confidence 차 ≤ 1e-6(6자리 반올림 경계) · rpi5-internal 스키마 통과 |
| 스트림 게이트 (10,692열, 28,180 update, 60/80/100% 단어 prefix + 마지막 final 변형) | 이벤트 열 전부 일치, 확정(DETECTED) 8,582건 동일 |
| 테스트 분할 스트림 지표 | positive_recall 0.8611, 오탐 0.0 — `artifacts/metrics.json`의 Python 값과 같음 |
| PARTIAL_INTENT_HINT | 341건 perception 스키마·registry(zone `living`, MIC-INDOOR-01 → NODE-RPI5-02, 후보 장치 등록) 통과 |

arm64(QEMU, `homemind-native-dev:24.04-arm64`): 같은 11개 시험 통과, 위 표의 숫자가 x86과 완전히 같다(특징 비트 일치 4,192, 확률 차 2.4e-7, 게이트 확정 8,582).

## 지연 (PC, 참고용)

predict 1회(정규화·특징·ONNX·이벤트), 1,000회, Release: **C++ P50 0.034 ms / P95 0.062 ms**, Python P50 0.42 ms / P95 0.68 ms. Pi 5 값 아님.

## Python 버그·주의 (패리티는 유지)

1. `cli.py stream`은 stdin 전체에 게이트 하나를 쓴다. `_emitted`가 초기화되지 않으므로 한 번 낸 의도는 이후 **다른 발화에서도 계속 SUPPRESSED**(utterance_id는 trace_id로만 씀). C++ `StreamIntentGate`는 같은 동작을 유지하고, 서비스용으로 `UtteranceIntentStream`(utterance_id마다 새 게이트)을 따로 제공
2. 게이트 기본 `confirmation_updates=2`는 생성자 기본값이고 metadata `streaming_confirmation_updates`를 읽지 않는다(지금은 둘 다 2라 차이 없음)
3. `text_sha256`은 짧은 정규화 문장의 해시라 흔한 명령은 사전 대입으로 역추적 가능. 내부 이벤트 전용으로 두는 것이 맞다(MQTT perception에는 넣지 않음)
4. Python 3.12 unicodedata는 Unicode 15.0, utf8proc 2.9는 15.1. 시험 입력에서 차이 없음. `lower()`의 Cased/Case_Ignorable(final sigma 판정)은 utf8proc 분류로 근사

## 의존성

- 빌드: `libutf8proc-dev`(dev 이미지에 있음), ONNX Runtime 1.20.1(선택, 없으면 특징 코드만 빌드되고 `IntentRuntime`이 예외)
- 실행 .so: libutf8proc.so.3, libonnxruntime.so.1
- 모델: `services/edge-ai/partial-intent/artifacts/partial-intent-v2.onnx` + `.json`(147,769 bytes, git 미포함). 패키징에서 복사

## 시험

```powershell
$env:HM_NATIVE_IMAGE = "homemind-native-dev:24.04"; $env:HM_NATIVE_BUILDER = "vision"
services\edge-ai\partial-intent\.venv\Scripts\python -m pytest native\tests\test_partial_parity.py -s -p no:cacheprovider --basetemp "$env:TEMP\hmpt\d1"
```

gtest `hm_partial_tests` 10개(해시, 정규화, 잘못된 UTF-8, 특징, 정책, 반올림, 이벤트에 원문 없음, 게이트, 발화별 게이트, 힌트 perception).
