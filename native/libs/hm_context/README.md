# hm_context — 오디오 컨텍스트 경로 (C++ 네이티브)

`services/runtime/src/homemind_runtime/audio/context.py`(AudioContextPipeline)와 그것이 쓰는
`services/edge-ai/audio-context`(YAMNet 임베딩·VAD 특징·activity 헤드·feature event),
`services/edge-ai/context-fusion`(정책·엔진·FeatureStreamFusion)을 C++20으로 옮긴 라이브러리.
RPI5(LG AI Native OS, aarch64, glibc 2.39)에는 Python·TensorFlow·Docker가 없으므로 YAMNet은 ONNX Runtime
1.20.1 C++ API로 돌린다. 인터페이스 계약은 `include/hm/context_pipeline.hpp`(agent B의 homemind-perception이 호출).

> 이 문서의 모든 수치는 **PC x86_64 Docker**(Ryzen 9 5900X) 또는 **QEMU arm64 에뮬레이션** 결과다. 실기(RPI5) 결과가 아니다.

## 파이프라인

```
4 s window (16 kHz float32, 1 s hop)
 ├─ YAMNet ONNX (waveform → [8, 1024] embeddings; 패딩·STFT·mel·log·0.96 s 패치 포함)
 │    └─ pooling (feature_version: mean-std-max-vad-v2 → mean|std|max = 3072)
 ├─ VAD temporal features (12개, NumPy float32 연산 재현)       → feature 3084개
 ├─ activity head (StandardScaler + multinomial LR × 2, 50/50 ensemble) → label / confidence / class_probabilities
 ├─ prediction_to_feature_event → AUDIO_CONTEXT (contracts/rpi5-internal/context-feature-observation)
 ├─ (presence_zone != UNKNOWN) 설정값 PRESENCE event, simulated=true / pc.scenario (Python과 동일)
 └─ FeatureStreamFusion(ContextFusionEngine(policy)) → ACTIVITY_CONTEXT (context-activity-event)
```

## 파일

| 경로 | 내용 |
|---|---|
| `include/hm/context_pipeline.hpp` | 계약: `hm::AudioContextPipeline`, `ContextPipelineOptions`, `ContextUpdate` |
| `include/hm/context/audio_features.hpp`, `src/yamnet.cpp`, `src/vad.cpp` | YAMNet ORT 래퍼(threads 옵션), pooling, VAD 특징 |
| `include/hm/context/activity_head.hpp`, `src/activity_head.cpp` | 헤드 JSON 로더, predict_proba, `prediction_to_feature_event` |
| `include/hm/context/fusion.hpp`, `src/fusion.cpp` | ContextObservation(from_mapping), classify, ContextFusionEngine, ContextFusionPolicy, FeatureStreamFusion, rpi5-internal 스키마 검증 |
| `include/hm/context/pyfmt.hpp`, `src/pyfmt.cpp` | Python 호환 `round()`, `f"{x:.Nf}"`, `repr(float)`, `datetime.fromisoformat`/`isoformat` |
| `src/context_pipeline.cpp` | 계약 구현 |
| `tools/export_yamnet_onnx.py` | SavedModel → ONNX (tf2onnx) + 정규화 + TF 대비 검증 |
| `tools/export_activity_head.py` | activity.joblib → JSON + sklearn 대비 검증 |
| `tools/hm_context_probe.cpp` | PC parity용 JSONL probe |
| `tests/test_context.cpp` | gtest (포맷/시간, VAD, fusion 시나리오, 모델 로드) |
| `../../tests/test_context_*.py`, `../../tests/context_support.py` | Python 대비 parity 테스트 (a)–(e) |

## 모델 export (산출물은 git에 넣지 않는다, `native/.deps/` 아래)

YAMNet은 **ONNX (tf2onnx 1.17.0, opset 17)** 로 변환했다. 근거: tf2onnx가 TF 2.20 SavedModel을 그대로 변환했고
(STFT는 DFT MatMul로, 패딩/프레이밍은 Shape/Pad/Gather로), ORT 1.20.1에서 TF와 cosine ≥ 0.99999999993이 나와서
log-mel 프론트엔드를 C++로 따로 짤 필요가 없다. C++ 쪽은 Python `model(waveform)`과 같은 16 kHz 파형을 넣는다.
tf2onnx 출력은 실행마다 노드 순서·이름이 달라서, export 스크립트가 구조 기반 DFS로 이름/순서를 정규화한다
(같은 SavedModel → 같은 바이트, 두 번 export해서 SHA-256 동일 확인).

```bash
# export 전용 venv (runtime venv는 건드리지 않음)
py -3.12 -m venv native/.deps/venv-export
native/.deps/venv-export/Scripts/python -m pip install tensorflow==2.20.0 tf2onnx==1.17.0 onnx onnxruntime==1.20.1
native/.deps/venv-export/Scripts/python native/libs/hm_context/tools/export_yamnet_onnx.py
# 헤드는 runtime venv (sklearn + homemind_audio, 읽기만 함)
services/runtime/.venv/Scripts/python native/libs/hm_context/tools/export_activity_head.py
```

| 산출물 | 크기 (bytes) | SHA-256 |
|---|---|---|
| `native/.deps/models/yamnet/yamnet-embeddings.onnx` | 13912384 | `869c10bdf8f8b16114a7e0634623b7acf4a2b128c11a108eb41aa420aa98fa10` |
| `native/.deps/models/yamnet/yamnet-embeddings.onnx.json` (export 보고서) | — | — |
| `native/.deps/models/activity-head/public-balanced-v6-ensemble-50-50.json` | 893062 | `81af0b9ec33122549e9e43153d05ea022b66cd73a90d9b60679ba200de734b18` |

원본: YAMNet SavedModel `saved_model.pb` SHA-256 `672af6e1…d462`, variables `d6753f22…4ddc`;
activity.joblib (`public-balanced-v6-ensemble-50-50`, model_version `yamnet-temporal-cost-ensemble-v0.8.0-50-50`,
feature_version `mean-std-max-vad-v2`, classes IN_PERSON_MEETING/SELF_TALK/TV_MEDIA/UNKNOWN/ZOOM_SPEAKER, weights 0.5/0.5).
헤드 JSON 포맷 `hm-activity-head/1`: members[{scaler{mean,scale}, logistic{coef,intercept,proba}}], ensemble.weights,
float는 Python repr(정확한 왕복).

## 빌드·런타임

- CMake: `HM_ONNXRUNTIME_ROOT`(include/, lib/)가 없으면 `hm_context: skipped` 메시지만 내고 configure는 계속된다.
- 타깃: `hm_context`(static), `hm_context_probe`, `hm_context_tests`. 링크: `hm_common`, `libonnxruntime.so`.
- `-ffp-contract=off -fno-fast-math`: aarch64 GCC는 기본으로 `a*b+c`를 FMA로 합쳐서 NumPy와 비트 단위가 달라진다.
  VAD·스케일러·fusion 산술의 비트 일치를 위해 필요하다(QEMU arm64에서 확인).
- ORT 세션: intra-op threads = `options.threads`(기본 2), inter-op 1, sequential, `ORT_ENABLE_ALL`,
  `session.intra_op.allow_spinning=0`(창 사이 1 s 동안 스레드가 CPU를 태우지 않게).
- 실행 의존성(ldd, x86 probe): `libonnxruntime.so.1`(SONAME, → `libonnxruntime.so.1.20.1` 번들 필요),
  `libstdc++.so.6`, `libm`, `libgcc_s`, `libc`; ORT가 `libdl/librt/libpthread`(glibc). 실행 파일 GLIBC 요구 ≤ 2.38.
  Pi 번들: `onnxruntime-linux-aarch64-1.20.1`의 `libonnxruntime.so.1.20.1` + `libonnxruntime.so.1` 심볼릭 링크
  (`libonnxruntime_providers_shared.so`는 CPU EP만 쓰므로 불필요).

## Parity 결과 (PC x86_64 Docker, Python 기준 구현 대비)

실제 오디오: AIHub test split(`public-balanced-v6-split.csv`) 5개 라벨 × 6개 = 30개 클립(E:\HomeMind-Datasets).

| 항목 | 결과 |
|---|---|
| (a) YAMNet 임베딩, 30 클립 437 프레임 | min cosine 0.99999999993, mean 0.99999999999, max abs diff 1.6e-5 |
| (a) VAD 12 특징, 155 창(클립 전체 + 4 s 창) | 1860 값 **비트 일치** (불일치 0) |
| (a) C++ 특징+C++ 헤드 vs TF 특징+sklearn 헤드, 155 창 | 라벨 155/155 일치, 특징 max diff 2.1e-5, confidence max diff 5.6e-6 |
| (b) 헤드 predict_proba, 동일 float32 입력 355 행 | max abs diff 1.3e-14, 라벨 동일 |
| (c) FeatureStreamFusion, 18 스트림 4962 이벤트 (scenarios/, artifacts/*.jsonl, 랜덤 시드 스트림 × 정책 3종) | debug record 4962/4962 동일(불일치 0; fused 2214, 거부 299건 종류 동일, ACTIVATED 30/DEACTIVATED 15/UPDATED 405) |
| (c) ContextFusionEngine + classify (demo.jsonl + 랜덤 600, from_mapping flat/nested) | 3642 레코드 동일 |
| (c) round/fixed/repr/isoformat/fromisoformat 헬퍼 | 5010 값 Python과 동일 |
| (d) 전체 파이프라인, 230.8 s 실제 오디오 227 창 (presence UNKNOWN / DESK 두 번) | raw label 227/227, 전이 13개 순서 동일, context_type·quiet_required 전부 동일, speech_ratio 227/227 비트 일치 |
| (d) 이벤트 필드 | feature event: 숫자 외 필드 전부 동일, 반올림된 값 max diff 1.9e-5(ONNX↔TF 수치 차이, 92/227 완전 동일). context event: 224/227 완전 동일, 나머지 3개는 `margin:0.458↔0.457`, confidence 0.698↔0.6981 (같은 원인) |
| (d) Python feature event를 C++ fusion에 재생 | 227/227 완전 동일 → 차이는 전부 YAMNet 추론 수치 |

**QEMU arm64 에뮬레이션**(`homemind-native-dev:24.04-arm64`, aarch64 ORT 1.20.1, 같은 소스·플래그 — 기능 확인만):
(a) cosine min 0.99999999993 / max abs 1.66e-5, VAD 1860/1860 비트 일치, 라벨 155/155, (b) 1.3e-14,
(c) 4962 이벤트·3642 레코드 불일치 0, (d) presence UNKNOWN 227 창: raw label 227/227, 전이 13개 동일, speech_ratio
227/227, context event 224/227 완전 동일(x86과 같은 3개 창), Python feature event 재생 227/227.

## 지연 (PC x86_64 Docker, Ryzen 9 5900X, 4 s 창, 60회)

다른 작업이 없을 때(첫 값) / 다른 Docker 작업과 겹쳤을 때 최댓값:

| threads | YAMNet p50 / p95 | VAD | head | fusion | 합계 p50 / p95 |
|---|---|---|---|---|---|
| 1 | 12.6 / 15.8 ms (≤16.1 / 24.1) | 0.09 ms | 0.06 ms | 0.09 ms | 12.9 / 16.1 ms (≤16.4 / 24.7) |
| 2 | 8.0 / 9.2 ms (≤10.9 / 17.1) | 0.09 ms | 0.05 ms | 0.08 ms | 8.2 / 9.5 ms (≤11.1 / 18.1) |
| 4 | 6.4 / 7.2 ms (≤9.7 / 11.4) | 0.09 ms | 0.06 ms | 0.08 ms | 6.6 / 7.4 ms (≤10.0 / 11.7) |

`process()` 전체(파이프라인 테스트, 2 threads, presence 이벤트 포함): 평균 9–15 ms, p95 12–19 ms.
Python(TF 2.20 CPU) 같은 경로: 창당 20–64 ms.

**RPI5 추정(측정 아님)**: 창당 연산량 ≈ 1.3 GFLOP(YAMNet 69.2M MAC/패치 × 8 + DFT MatMul 89M + mel 7M MAC).
Cortex-A76 2.4 GHz NEON(코어당 FP32 FMA 처리량이 Zen3 AVX2의 약 1/4)과 메모리 대역폭을 감안해 PC 대비 4–8배 →
2 threads 기준 창당 약 40–80 ms(YAMNet), 나머지 단계 1–2 ms. 1 s hop 예산 안이지만 같은 노드의 whisper와 CPU를
나눠 쓰므로 실기에서 반드시 측정해야 한다(QEMU 시간은 의미 없음).

## 테스트 실행

```bash
export HM_NATIVE_BUILDER=context   # 자기 Docker 빌드 볼륨
services/runtime/.venv/Scripts/python -m pytest native/tests/test_context_models.py native/tests/test_context_fusion.py \
  native/tests/test_context_pipeline.py native/tests/test_context_latency.py -p no:cacheprovider --basetemp "$TEMP/hmpt/c1" -s
# QEMU arm64 (기능 확인만, 속도 의미 없음)
HM_NATIVE_IMAGE=homemind-native-dev:24.04-arm64 HM_NATIVE_BUILDER=context-arm64 services/runtime/.venv/Scripts/python -m pytest ...
```

- 공용 `/build` configure가 다른 폴더의 작업 중 코드 때문에 깨지면, 테스트는 `hm_common + hm_context`만 심볼릭
  링크한 별도 소스 루트로 `/build/hm_context-only`에 빌드한다(`context_support.ensure_built`).
- 실제 오디오가 필요한 테스트는 E: 데이터가 없으면 skip. 모델이 없으면 skip(먼저 export).
- probe 데이터·보고서: `native/.deps/tmp/context-parity-<builder>/` (`*.json`에 각 항목 요약).

## Python 기준 구현과 맞춘 세부 (주의점)

- VAD: NumPy pairwise summation(8 누산기, 블록 128), float32 + Python 약한 스칼라(NEP 50) 규칙, percentile의
  float32 보간, threshold의 float32 캐스팅까지 재현. log10은 `(float)log10((double)x)`(실측 비트 일치).
- 스케일러: sklearn이 float32 입력을 float64로 계산 후 float32로 저장하는 동작을 재현.
- 반올림/문자열: Python `round()`·`format` 은 이진값 기준 정확 반올림(ties-to-even) → glibc printf + strtod로 동일.
- 시각: feature event는 `datetime.isoformat()`(`+00:00`, 마이크로초 0이면 생략), context event는
  `isoformat(timespec="milliseconds")` + `Z`(절사). 입력은 `fromisoformat` 규칙(오프셋 → UTC, naive → UTC).
- 스키마 검증: rpi5-internal 두 스키마를 손으로 구현. runtime venv에 `rfc3339-validator`가 없어서 Python도
  date-time format을 검사하지 않으므로 C++도 검사하지 않는다(파싱 실패는 둘 다 ValueError).

## 기준 구현 관찰 사항 (parity 유지, 수정하지 않음)

- `duration_seconds`는 스트림 시작 후 경과 시간(perception `elapsed_seconds`, context-fusion `audio_pipeline.py`도 동일)이라
  가동 20 s 이후 모든 창에 `duration:sustained` 가점(TV/ZOOM/MEETING +0.08)이 붙는다. 컨텍스트 지속 시간 의미라면
  Python·계약을 먼저 바꿔야 한다.
- AUDIO_CONTEXT `class_probabilities` 스키마는 5개 라벨만 허용(QUIET·ZOOM_HEADPHONES 없음). 지금 헤드 클래스와 맞지만,
  그 라벨을 내는 헤드로 바꾸면 feature event가 계약 위반이 된다.
- `presence_zone` 설정 시 PRESENCE는 항상 `pc.scenario`/`simulated=true`라서 모든 fused 이벤트 evidence에
  `input:simulated:...presence`가 붙는다(계약 헤더대로 Python과 동일).
- 생성자 예외: 모델 파일 없음 → `std::runtime_error`, 정책 미승인·알 수 없는 필드·잘못된 presence_zone →
  `std::invalid_argument`(Python은 잘못된 zone을 첫 창에서야 거부; C++은 생성 시 거부).

## 추가 API (2026-10-22, agent F, 하위 호환): 카메라 재실 입력

`AudioContextPipeline::submit_presence(json)` · `pending_presence()` · `dropped_presence()`, `ContextUpdate::presence_events`.
기존 호출(`process`)과 `presence_zone` 고정값 동작은 그대로다.

- homemind-vision의 rpi5-internal `PRESENCE` 관측(context-feature-observation)을 다른 스레드에서 넣을 수 있다(스레드 안전).
  넣을 때 `validate_feature_event` + `event_type == PRESENCE` + `valid_until > timestamp`를 검사한다(위반 시 예외, 창 처리에는 영향 없음).
- `presence_zone` 고정값이 설정된 파이프라인에는 넣을 수 없다(`std::logic_error`): 두 입력을 동시에 쓰지 않는다.
- 대기열(최대 64, 넘치면 가장 오래된 것 버림)은 timestamp 순으로 정렬되고, **창 timestamp 이하인 것만** 그 창의 오디오 이벤트
  직전에 fusion에 넣는다. 오디오 창이 fusion 시계이므로, 창보다 새로운 재실을 바로 넣으면 `FeatureStreamFusion`이 그것을
  `future`로 보고 직전의 유효한 재실까지 잃는다(이 판단은 네이티브 전용, Python 기준 구현에는 실시간 재실 입력이 없다).
- `ContextUpdate::presence_events` = 그 창 직전에 넣은 재실과 fusion 결과(`buffered`/`duplicate`/`out_of_order`).
  PC 시험은 이 기록을 Python `FeatureStreamFusion`에 같은 순서로 다시 넣어 fused event가 같은지 본다
  (`native/tests/test_perception_live.py`: 스크립트 재실 72창 72/72, vision → FIFO 21창 21/21 동일, PC x86_64 Docker).
