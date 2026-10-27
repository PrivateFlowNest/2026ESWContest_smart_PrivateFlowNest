# HomeMind Partial Intent

라즈베리파이 없이 PC에서 먼저 검증하는 한국어 부분 발화 의도 분류기입니다. 스트리밍 ASR의 중간 텍스트를 입력받아 작은 ONNX 모델로 의도를 분류하고, 실행 명령이 아닌 **읽기 전용 준비 후보**만 JSON으로 냅니다.

## 현재 범위

- 9개 분류: 온도, 조명, 환기, 미디어, 출입, 세탁, 청소, 정숙 환경, 동작 없음
- 한국어 문자 n-gram + 로지스틱 회귀를 단일 ONNX 파일로 내보냄
- 연속 두 번 같은 의도가 관측돼야 중간 발화 이벤트를 확정
- 원문은 이벤트에 넣지 않고 SHA-256과 길이만 기록
- `execution_allowed`는 항상 `false`

예를 들어 `오늘 저녁에는 방이 좀 추울 것 같은데`는 `CLIMATE_COMFORT`로 분류하지만, 난방을 켜라는 명령은 만들지 않습니다. 온도·재실·기기 상태를 확인할 후보만 냅니다.

## PC 기준선 만들기

PowerShell에서 다음을 실행합니다.

```powershell
cd "C:\Users\alexi\Desktop\Workspace\ESW\services\edge-ai\partial-intent"
& .\tools\run_pc_baseline.ps1
```

결과는 `artifacts/partial-intent-v2.onnx`, `artifacts/metrics.json`, `artifacts/metrics-final-pc-v3.json`, `artifacts/benchmark-pc.json`에 생성됩니다. `metrics-final-pc-v3.json`은 모델과 정책을 고정한 뒤 만든 별도 한국어 문장 60개의 PC 최종 결과입니다.

최종 PC 텍스트 기준선은 원시 ONNX 정확도 93.3%, 정책 적용 정확도 98.3%입니다. 스트리밍 대상 의도 재현율은 100%이고 비대상 제안 오탐은 20개 중 1개(5%)입니다. 부분 의도 계약은 실행 명령 생성을 금지하므로 장치를 실제로 잘못 움직이는 오탐률은 0%입니다. 실제 ASR과 Pi 5 수치는 하드웨어 준비 후 별도로 측정해야 합니다.

단일 문장은 PC 기준선 실행 후 다음과 같이 확인할 수 있습니다. 현재는 이미 설치된 비전 작업용 Python 환경을 함께 사용합니다.

```powershell
$env:PYTHONPATH = "$PWD\src"
& "..\vision-presence\.venv\Scripts\python.exe" -m homemind_partial_intent.cli predict "오늘 저녁에는 방이 좀 추울 것 같은데"
```

## 아직 합격 판정이 아닌 것

현재 정확도는 사람이 만든 합성 문장의 의미 그룹을 분리한 테스트 결과입니다. 실제 성능 판정에는 이후 대회용 ASR이 내놓는 중간 문장 로그가 필요합니다. 하드웨어가 준비되면 실제 원룸 소음 조건의 로그를 고정 평가셋으로 추가하고, 같은 지표를 Pi 5에서 다시 측정합니다.
