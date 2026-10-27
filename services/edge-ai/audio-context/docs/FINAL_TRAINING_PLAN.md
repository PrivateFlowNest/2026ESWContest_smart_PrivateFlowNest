# HomeMind 최종 오디오 모델 학습 기준

기준 문서: Notion `03. AI·프라이버시·모바일 전략`의 자동 생활 맥락 추론,
피드백 학습, 안전 배포 항목.

## 최종 모델로 부를 수 있는 조건

1. 활동 6개 평가 클래스와 음향 이벤트 3개 및 `OTHER`가 모두 존재한다.
2. 원본 녹음·화자·세션 단위로 train/validation/test가 완전히 분리된다.
3. test는 증강하지 않은 독립 현장 녹음만 사용한다.
4. 활동과 음향 이벤트 모두 test Macro F1 0.90 이상을 달성한다.
5. TV·미디어↔회의 혼동행렬, 클래스별 오탐률, 활성·해제 지연을 기록한다.
6. 실제 Pi 5에서 지연·메모리·온도 시험을 통과한다.
7. 모델 manifest, SHA-256, 데이터 라이선스, rollback 버전을 함께 배포한다.

`homemind-audio readiness --manifest <manifest.csv>`가 구조적 결손을 검사한다.
이 검사를 통과해도 성능과 Pi 5 시험을 통과하기 전에는 최종 모델이 아니다.

## 데이터 역할

| 목표 라벨 | 주 데이터 | 현재 상태 |
|---|---|---|
| `IN_PERSON_MEETING` | AI Hub 464 대면 회의 | 확보 |
| `ZOOM_SPEAKER` | AI Hub 464 비대면 회의 | 확보 |
| `TV_MEDIA` | AI Hub 463 방송 | 확보 |
| `SELF_TALK` | AI Hub 109 단독 발화 | 확보 |
| `QUIET` | 실제 원룸 배경음 | 현장 수집 필요 |
| `ZOOM_HEADPHONES` | 실제 헤드폰 통화 세션 | 현장 수집 필요 |
| `DOORBELL` | FSD50K + 실제 시연 초인종 | FSD50K 확보·현장 test 필요 |
| `ALARM` | AI Hub 71698 + FSD50K | 공개 원본 확보 |
| `WASHER_COMPLETE` | 실제 시연 세탁 완료음 | 현장 수집 필요 |
| `OTHER` | AI Hub/FSD50K 혼동 음원 | 공개 원본 확보·현장 보강 |

### 공개 데이터 v3 보강

- TAU Sound Events and Speech Privacy Preservation: 음성과 겹친 초인종으로
  `DOORBELL`을 보강한다. FSD50K 원본 ID와 split을 재사용하고 eval 파생본은 제외한다.
- ReaLISED: 실제 실내 세탁기 작동음, TV, 문, 가전 소리를 `OTHER` 하드 네거티브로
  사용한다. 이 데이터로 `WASHER_COMPLETE`가 확보됐다고 간주하지 않는다.
- 두 데이터 모두 원본 라이선스와 출처를 매니페스트에 남긴다.

## 현장 원본 수집 단위

한 CSV 행이 아니라 `group_id`로 표시되는 독립 원본 세션 수를 기준으로 관리한다.
최소 구조 게이트는 클래스마다 train 80, validation 15, test 20개 원본이다.
같은 원본에서 자른 조각이나 같은 음원에 잡음만 바꾼 증강본은 원본 하나로 센다.

### QUIET

- 빈 방, PC 팬, 창문 열림/닫힘, 공기청정기, 외부 교통음 등 실제 원룸 배경을 포함한다.
- 완전한 디지털 무음은 사용하지 않는다.
- 시간대·거리·창문 상태를 바꿔 8초 이상 독립 녹음한다.

### ZOOM_HEADPHONES

- 사용자는 헤드폰을 쓰고 상대 음성은 방에 나오지 않는 상태로 실제 대화를 한다.
- 한 사람이 문장을 읽기만 하는 `SELF_TALK`와 달리 질문·응답·긴 침묵·짧은 맞장구를 포함한다.
- 통화 한 번 전체를 하나의 `group_id`로 두고, 같은 통화 조각을 split 사이에 섞지 않는다.

### WASHER_COMPLETE

- 시연에서 실제로 재생할 완료음을 원본 신호로 고정한다.
- 원본 파일만 복제하지 말고 방에서 스피커로 재생한 소리를 여러 거리·방향·볼륨·배경음
  조건으로 다시 녹음한다.
- validation/test에는 학습에서 사용하지 않은 세션·거리·배경 조건을 둔다.

### DOORBELL

- FSD50K의 CC0/CC-BY `Doorbell`을 사용하고 저작자·라이선스·원본 URL을 보존한다.
- 최종 test에는 실제 시연 초인종을 방에서 재생해 다시 녹음한 원본을 포함한다.

## 증강 규칙

- 증강은 train에만 적용한다.
- 허용: gain, 거리 감쇠, 실제 원룸 배경 혼합, 가벼운 EQ, 제한된 잔향.
- 금지: validation/test 증강, 같은 원본의 split 간 중복, 라벨을 바꾸는 과도한 변형.
- 증강본은 원본 `group_id`를 그대로 유지한다.

## 학습 순서

1. 공개 원본과 현장 원본을 하나의 manifest로 병합한다.
2. readiness 게이트와 라이선스 검사를 통과시킨다.
3. YAMNet + VAD 특징을 미리 계산하고 캐시한다.
4. 클래스 균형 학습과 하이퍼파라미터 탐색은 validation으로만 수행한다.
5. 설정을 고정한 뒤 test를 한 번 평가한다.
6. Pi 5용 TFLite YAMNet과 소형 분류기 패키지를 생성한다.
7. 실제 마이크에서 활성·해제 지연과 연속 오탐을 측정한다.

## 현재 기준선

- YAMNet 평균: 활동 정확도 81.66%, Macro F1 81.67%.
- YAMNet + VAD: 활동 정확도 83.50%, Macro F1 83.75%.
- YAMNet + VAD: 경보/기타 정확도 94.14%, Macro F1 93.29%.
- 남은 핵심 병목은 TV·미디어를 회의로 오인하는 경우와 현장 클래스 결손이다.

## 공개데이터 전체 목록 · 2026-08-28

- `data/public-full.csv`: AI Hub 868,518행 + FSD50K 3,861행 = 872,379행.
- 완전히 같은 중복 행 0개, split 간 `group_id` 누수 0개, 빈 `group_id` 0개.
- 최소 수량 판정은 조각 행 수가 아닌 라벨·split별 고유 `group_id` 수를 사용한다.
- `DOORBELL` train은 79개로 기준보다 1개 적지만 공개데이터 단계에서는 승인된 예외로
  기록하고, 실제 시연 초인종 현장 녹음으로 보완한다.
- 공개데이터에 없는 `QUIET`, `ZOOM_HEADPHONES`, `WASHER_COMPLETE`와 활동 클래스의
  독립 test는 Pi 5/대회용 마이크가 준비된 뒤 현장 원본으로 추가한다.
- 이 목록은 전체 원본 인덱스다. 643,604개의 `SELF_TALK` 조각을 그대로 한 epoch에
  투입하지 않고, 다음 단계에서 `group_id` 단위 균형 샘플링과 특징 캐시를 적용한다.

## 공개데이터 기준선 학습 설정

- 입력: `data/public-balanced-v1.csv`
- 선택: 라벨별 train 최대 4,000행, validation/test 최대 1,000행.
- 원본 다양성: 같은 `group_id`에서 최대 4개 조각만 사용하고 모든 그룹의 첫 조각을
  우선 선택한 다음 두 번째 조각을 선택한다.
- 캐시: `cache/public-balanced-v1.sqlite3`에 YAMNet+VAD 특징을 누적해 중단 후 재개한다.
- 평가는 validation과 test를 분리하며 공개 test는 기준선 비교용일 뿐 최종 현장
  독립 test를 대체하지 않는다.

## 공개데이터 v2 재학습 설정

- 입력: `data/public-balanced-v2.csv`.
- 같은 라벨 안에서 AI Hub와 FSD50K를 라운드로빈한 뒤 라벨 상한을 채운다.
- `OTHER` train의 FSD50K 비중은 570개에서 전체 1,387개로 늘고, 기존 특징 캐시
  31,369개를 재사용하며 새 특징 853개만 계산한다.
- 출력은 `path`, `start_seconds` 순서로 유지해 E 드라이브 HDD의 탐색을 줄인다.
- RTX 3090 혼합 길이 벤치마크에서 batch 8이 101.09 samples/s로 선택됐고,
  batch 1의 36.81 samples/s 대비 약 2.75배였다.
- batch 1/2/4/8/16 모두 단일 추출 특징과 최대 절대 오차 0으로 확인됐다.
- CPU prefetch worker 2개, read-ahead 2배치로 제한해 HDD 랜덤 읽기 경쟁을 억제한다.
- 공개데이터에는 `QUIET`, `ZOOM_HEADPHONES`, `WASHER_COMPLETE`가 없으므로 v2도
  최종 모델이 아니라 공개데이터 개선 기준선이다.

## 공개데이터 v3 재학습 설정

- `data/public-balanced-v3.csv`에 TAU 초인종과 ReaLISED 하드 네거티브를 추가한다.
- 총 32,414개 샘플이며 v2의 특징 30,482개를 그대로 재사용하고 새 특징 1,932개만
  계산한다.
- `DOORBELL`은 train 234개/고유 원본 85개, validation 54개/고유 원본 19개로
  공개데이터 구조 기준을 통과한다. 기존 FSD50K test 27개는 그대로 유지한다.
- `OTHER` train은 AI Hub 1,333개, FSD50K 1,333개, ReaLISED 1,334개로 균형화한다.
- 기존 FSD50K와 TAU는 같은 원본 그룹을 공유하므로 최대 4개 파생 조각만 선택한다.
- 전체 그룹 split 누수와 누락 파일은 0건이다.
- RTX 3090 설정은 검증된 batch 8, CPU prefetch worker 2개, read-ahead 2배치를 유지한다.
- v1 특징 캐시를 재사용하고 새 파일만 YAMNet 특징을 계산한다.
- v3 역시 `WASHER_COMPLETE`와 현장 독립 test가 없으므로 최종 대회 모델은 아니다.

## 공개데이터 v4 시간 특징 실험

- 입력 데이터는 v3와 동일하게 `data/public-balanced-v3.csv`로 고정해 특징 정의의
  효과만 비교한다.
- 기존 평균 YAMNet 1,024차원 대신 프레임별 평균·표준편차·최대값 3,072차원과
  VAD 12차원을 결합한다.
- v1-v3 모델의 예측 호환성과 캐시는 보존하고 v4 전용 cache identity와 SQLite를
  사용한다.
- Logistic Regression은 `C=0.03,0.1,0.3,1,3,10`과 class weight
  `balanced/none`을 validation Macro-F1로만 비교한다.
- test는 선택에 사용하지 않고 설정을 고정한 뒤 한 번 평가한다.
- RTX 3090 32개 혼합 길이 벤치마크에서 batch 8이 143.04 samples/s로 선택됐고,
  batch 1/2/4/8/16 모두 단일 특징과 최대 절대 오차 0으로 확인됐다.
- cache scan은 500개마다 hit/miss 진행률을 출력해 WSL→HDD 메타데이터 조회 중에도
  정지 상태와 구분할 수 있게 한다.
