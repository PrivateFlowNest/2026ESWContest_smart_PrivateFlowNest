# EXAONE v3 → ExecuTorch `.pte` (system-architecture 3-2, 2안 병행 시험)

모든 결과는 **PC 시뮬레이션**(Windows + WSL2 Ubuntu 22.04, Ryzen 9 5900X, CPU 전용) 또는 **QEMU 기능 확인**이다.
Pi 5 실기 결과가 아니며 지연 시간은 기기 수치로 인용하지 않는다.

## 결론 요약

| 항목 | 결과 |
|---|---|
| 변환 | **성공**. LoRA v3 병합 HF 체크포인트(Exaone4ForCausalLM) → optimum-executorch(torch.export) → XNNPACK `.pte`, 8da4w(선형 int4 group 32 + 동적 int8 활성) + 임베딩 int8(행별) |
| 100문장 acceptance | **100/100** (llama.cpp v3 100/100) |
| 독립 v1 / v2 | **77/80 / 72/80** (v3 77/80 / 72/80), 안전 21/21·16/16·18/18, 숨은 의도 20/22 (v3 20/22) |
| 사례별 차이 | 260건 중 제안 1건만 다름(`fresh-context-070`: intent CONTEXT_SUGGESTION→DEVICE_CONTROL, 기대값 판정은 둘 다 통과). model_call·first-pass·retry·guard 수치 전부 동일 |
| PC 지연(같은 PC, CPU, 12스레드, 실제 페이로드 20건) | ET 8da4w p50 9.6 s / p95 10.1 s, llama.cpp Q4_K_M CPU p50 3.2 s / p95 3.4 s. (v3 보고서의 p50 ≈ 225 ms는 llama-server가 Vulkan으로 RTX 3090에 올린 값) |
| aarch64 | `llama_main`(ExecuTorch C++ LLM runner) 교차 빌드 성공, QEMU arm64에서 실행 확인 — 아래 참조 |
| 권고 | **1안(llama.cpp) 유지**. 2안은 PC(x86)에서 정확도 동등(전환 조건 충족)이지만 이득이 없다: PC CPU에서 더 느리고, 기기 경로에 서버·JSON 스키마 제약 디코딩·프롬프트 템플릿을 새로 만들어야 하며, aarch64 수치 차이 때문에 Pi 5 재평가도 필요 |

## 버전

| 구성 | 버전 / 출처 |
|---|---|
| venv | WSL `~/venvs/homemind-executorch` (Python 3.12.13, uv). `~/venvs/homemind-exaone`은 건드리지 않음 |
| torch / torchao | 2.12.0+cpu / 0.17.0+cpu (download.pytorch.org) |
| executorch | 1.3.1 (PyPI; optimum-executorch `install_dev.py` stable 스택과 동일) |
| transformers / optimum | 5.0.0rc1 / 2.0.0 |
| optimum-executorch | GitHub `huggingface/optimum-executorch@44fa1d333f` (0.2.0.dev0) |
| xgrammar | 0.2.8 (`--no-deps`, shim의 JSON 스키마 제약용) |
| aarch64 runner | `pytorch/executorch` tag v1.3.1 (commit e2f18eb23c45), Ubuntu 24.04 + g++-aarch64-linux-gnu 13, `-march=armv8.2-a+dotprod+fp16 -mtune=cortex-a76` |
| 비교 기준 | llama-server b10588 (70adb1b4c, winget) + `exaone-1.2b-homemind-v3-q4_k_m.gguf` |

## 산출물 (git 제외: `*.pte`, `*.log`, `native/.deps/`)

| 파일 | bytes | SHA-256 |
|---|---|---|
| `services/exaone-edge/artifacts/executorch/exaone-1.2b-homemind-v3-8da4w/model.pte` | 930,860,160 | `999b6b76b29e30eed226ad9f4bd3feb750456b6f6e8d485ebf21a907eac147b6` |
| `native/.deps/executorch/aarch64/bin/llama_main` | 6,964,064 | `44e36ecb35b2bf267f74bac0276084efe64fbad7de08aa116acca4a337005e5f` |
| `native/.deps/executorch/aarch64/bin/libllama_runner.so` | 4,247,161 | `31b389e8a47f14d9d22fef16d1fb39d30c931a28cce7eb530817538bb90cd408` |

`.pte` 메타데이터: max_seq_len 4096(llama-server `-c 4096`과 같음), 동적 prefill 길이, custom SDPA + custom KV cache(fp32), vocab 102400, EOS 361 `[|endofturn|]`.
내보내기: 9 분 28 초, 최대 RSS 8.8 GB (`artifacts/executorch/export-8da4w.log`).
작은 증거 파일(git 추적 대상이 될 수 있음, 모델 출력 JSON·타이밍만 있고 발화 원문 없음): `artifacts/executorch/*-trace.jsonl`(compat trace에는 지연 측정 21건도 포함), `latency-pc-*.json`, `eval-summary-*.json`, `qemu-aarch64-python-short-20261022.json`,
평가 보고서 `artifacts/exaone-q4-{acceptance,independent-v1,independent-v2}-{et,et-nogrammar,et-hf}-20261022.json`(파일명의 `q4`는 eval_candidate.py 규칙).

## 이 폴더

| 파일 | 역할 |
|---|---|
| `setup_venv.sh` | WSL venv 생성(버전 고정) |
| `export_pte.sh` | optimum-cli로 `.pte` 내보내기(`8da4w` 기본, `8da8w` 선택) |
| `et_server.py` | `.pte`를 OpenAI 호환 `/v1/chat/completions`로 노출하는 PC용 shim |
| `parity_check.py` | llama-server와 프롬프트·토큰 동일성 확인 |
| `latency_bench.py` | 같은 페이로드로 엔드포인트별 요청 지연 측정 |
| `pte_greedy_check.py` | 토큰 id 입력 → greedy 토큰 id (x86/aarch64 비교용, 토크나이저 불필요) |
| `Dockerfile.aarch64-runner`, `bundle_libs.sh` | ExecuTorch C++ `llama_main` aarch64 교차 빌드 |
| `Dockerfile.aarch64-pycheck` | 공식 executorch aarch64 휠로 QEMU 기능 확인 |

## 재현

```bash
# WSL
bash setup_venv.sh
bash export_pte.sh 8da4w          # -> artifacts/executorch/exaone-1.2b-homemind-v3-8da4w/model.pte
python et_server.py --pte <model.pte> --tokenizer ../../artifacts/exaone-1.2b-hf-merged-v3 \
    --port 8093 --threads 12 --prompt-mode llama-compat
# Windows (services/exaone-edge)
.venv/Scripts/python training/eval_candidate.py --label et --base-url http://127.0.0.1:8093 --model exaone-et
.venv/Scripts/python training/executorch/latency_bench.py --base-url http://127.0.0.1:8093 --model exaone-et --n 20 --out lat.json
# aarch64 runner (Docker Desktop)
docker buildx build -f Dockerfile.aarch64-runner --build-arg JOBS=8 \
    --output type=local,dest=../../../../native/.deps/executorch/aarch64 .
```

## shim (`et_server.py`)이 llama-server와 맞춘 것

- `/health`, `/v1/chat/completions`만. temperature 무시하고 항상 greedy, `max_tokens` 준수.
- 프롬프트: 병합 v3의 `chat_template.jinja` + 요청의 `chat_template_kwargs`(`enable_thinking: false`), BOS 없음.
  `parity_check.py`로 세 세트 전체 + 재시도 형태 288개 페이로드를 llama-server `/apply-template`·`/tokenize`와 비교: `--prompt-mode llama-compat` **288/288 토큰 동일**.
- `response_format.json_schema` → xgrammar 토큰 마스크(llama-server는 같은 스키마를 GBNF로 변환). 스키마의 속성 순서 유지.
- 이전 프롬프트와 공통 접두부는 KV cache 재사용(llama-server `cache_prompt`와 같은 방식).

## 발견 사항 (lead 확인 필요)

1. **llama-server b10588이 EXAONE 4 템플릿의 system 턴을 버린다.** GGUF에 들어간 템플릿은 `chat_template.jinja`와 글자 단위로 같고 `supports_system_role: true`인데,
   `/apply-template` 결과와 실제 `/v1/chat/completions`의 `prompt_tokens`(예: 1374)에 `SYSTEM_PROMPT`가 없다. HF Jinja2로 렌더하면 1915 토큰(system 포함).
   LoRA v3는 `train_lora.py`의 `apply_chat_template`(system 포함)으로 학습했으므로 **학습 형식과 서빙 형식이 다르다**. v3 100/100은 system 없는 상태에서 나온 값이다.
   원인: llama.cpp b10588의 Jinja 엔진이 `{%- continue %}` 직전에 같은 반복에서 출력한 내용을 버린다. 최소 템플릿으로 확인:
   `{% for m in messages %}{% if loop.index0 == 0 %}A:..;{% continue %}{% endif %}B:..;{% endfor %}` → `B:U1;B:X;B:U2;`(A 없음),
   같은 내용을 `{% else %}`로 쓰면 → `A:S;B:U1;B:X;B:U2;`. 즉 EXAONE 템플릿의 system 분기(출력 후 `continue`)만 사라진다.
   기기용 llama-server(`third_party/llama.cpp` 0f3a71b = b10760, arm64 교차 빌드)도 QEMU에서 `/apply-template`로 확인한 결과 **똑같이 system을 버린다** → PC와 기기의 입력은 같다.
   system을 넣은 형식(`--prompt-mode hf`, 아래 표)은 오히려 숨은 의도 20→17/22로 낮았다(ET 8da4w 기준, llama.cpp에서는 미측정).
   따라서 지금 숫자는 "system 없음"에 묶여 있다. llama.cpp를 이 Jinja 동작이 고쳐진 버전으로 올리면 입력이 조용히 바뀌므로, 버전을 고정하거나 다시 평가해야 한다.
2. v3 보고서의 PC 지연(p50 ≈ 225 ms)은 winget llama.cpp가 Vulkan으로 GPU에 올린 값이다(CPU 전용 `-ngl 0 --device none -t 12`: p50 3.2 s). CPU 수치로 오해하지 말 것.
3. executorch 1.3.1 x86_64 휠: `libquantized_ops_aot_lib.so`의 RUNPATH가 없는 경로(`extensions/pybindings`)를 가리켜 `executorch.kernels.quantized`가 조용히 로드 실패한다 → `portable_lib`을 먼저 import 하면 해결(shim에 반영).
4. optimum-executorch `text-generation` 작업은 `--qlinear_group_size`를 무시한다(항상 32).

## 추가 실험

| 실행 | acceptance | 독립 v1 | 독립 v2 | 안전 | 비고 |
|---|---|---|---|---|---|
| llama.cpp v3 (기준, 2026-10-22) | 100/100 | 77/80 | 72/80 | 전부 통과 | GBNF, system 없음 |
| ET 8da4w, llama-compat, xgrammar | 100/100 | 77/80 | 72/80 | 전부 통과 | 채택 비교 대상 |
| ET 8da4w, `--prompt-mode hf` (system 포함) | 100/100 | **76/80** | **71/80** | 전부 통과 | 학습 형식. 숨은 의도 17/22(기준 20/22): 3건이 NO_ACTION으로 바뀜, 대신 `indep2-command-005`는 회복 |
| ET 8da4w, llama-compat, `--no-grammar` | 100/100 | 77/80 | **71/80** | 전부 통과 | `indep2-command-025` 1건: 첫 출력·재시도 모두 스키마 위반 → fallback CLARIFY (llama.cpp는 재시도에서 복구) |

제약 디코딩이 없으면 1건이 떨어진다. 기기에서 2안을 쓰려면 JSON 스키마 제약도 같이 옮겨야 같은 결과가 나온다.

## aarch64 (QEMU, 기능 확인만 — 지연 수치 아님)

- 교차 빌드: `Dockerfile.aarch64-runner`(Ubuntu 24.04, gcc 13 aarch64, glibc 2.39 = LG AI Native OS scarthgap과 같은 기준).
  `llama_main` + `libllama_runner.so`(RUNPATH `$ORIGIN`), 필요한 GLIBC 최대 2.38. XNNPACK·custom SDPA/KV cache·quantized 커널·HF tokenizer 포함.
- `docker run --platform linux/arm64 ubuntu:24.04`에서 실제 클라이언트 프롬프트(1374 토큰, "아 좀 덥네", llama-compat)를 greedy로 실행:
  프롬프트 토큰 수 1374(llama-server와 같음), 출력 JSON
  `{"intent":"DEVICE_CONTROL","device":"AIR_CONDITIONER","action":"TURN_ON","parameters":{},"ambiguous":false,"safety_flags":[],"clarification_question":null}`
  = x86 ET·llama.cpp와 같은 문자열 (`artifacts/executorch/qemu-aarch64-llama_main-fullprompt.log`).
- 주의 1: `--tokenizer_path`에 `tokenizer.json` 파일만 주면 EOS(361)를 몰라 `[|endofturn|]` 뒤에도 생성이 이어진다.
  `.pte`의 메타 메서드는 `get_eos_id`(optimum)인데 runner v1.3.1은 `get_eos_ids`를 찾고, 없으면 토크나이저 EOS를 쓴다.
  `tokenizer.json` + `tokenizer_config.json`이 든 **폴더**를 주면 361에서 멈춘다(짧은 프롬프트로 확인, `qemu-aarch64-llama_main-short.log`).
- 주의 2: 짧은 자유 문장 프롬프트(27 토큰)에서는 aarch64(파이썬 휠·`llama_main` 둘 다 같은 출력)와 x86의 greedy 결과가 첫 토큰부터 갈렸다
  (`qemu-aarch64-python-short-20261022.json`). ISA별 XNNPACK 커널의 수치 차이 때문이다. 즉 x86에서 낸 260건 결과가 Pi 5에서 그대로라는 보장은 없다.
  QEMU에서는 prefill 약 0.9 tok/s라 260건을 돌릴 수 없으므로, 2안을 계속 본다면 Pi 5에서 같은 평가를 다시 해야 한다.

## 2안으로 기기에 올린다면 남는 일 (권고 근거)

- 기기 경로의 C++ LLM 노드는 llama-server HTTP(`/v1/chat/completions`, `response_format` json_schema)에 의존한다. ExecuTorch에는 서버가 없다:
  `TextLLMRunner`(또는 Module 직접 호출) 위에 OpenAI 호환 서버, EXAONE 템플릿 렌더링(C++에 Jinja 없음), JSON 스키마 제약 디코딩(xgrammar/llguidance C++ 연동), 접두부 KV 재사용을 새로 구현해야 한다.
- x86 PC에서는 XNNPACK 8da4w가 llama.cpp Q4_K_M보다 prefill 약 4.8배(70 vs 340 tok/s), decode 약 1.7배(15.6 vs 27 tok/s) 느렸다. Cortex-A76(dotprod) 상대 성능은 실기 측정 전에는 모른다.
- 정확도는 같으므로 2안으로 바꿀 근거는 "Pi 5 실측에서 ET가 확실히 빠르다"는 경우뿐이다. 그 측정은 이 폴더의 `.pte` + `llama_main`으로 할 수 있다(사용자 확인 후).
