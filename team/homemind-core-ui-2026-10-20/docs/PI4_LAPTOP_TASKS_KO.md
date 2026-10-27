# Pi4 + 랩탑 전체 작업 체크리스트

마지막 갱신: 2026-09-03

개발 구성:

- Pi4: `10.10.10.2`
- SSH: `ssh root@10.10.10.2`
- Pi4 MQTT TCP: `1883`
- 랩탑 Flutter 웹 MQTT WebSocket: `9001`
- 인증 MQTT 리스너: `1884` (현재 WebSocket 아님)
- 랩탑은 개발 중 가짜 Pi5와 가짜 ESP32 역할을 수행한다.

## 1. 기본 연결과 현재 상태

- [x] 랩탑에서 Pi4 ping 확인
- [x] Pi4 SSH 접속 확인
- [x] `homemind-mqtt` 실행 및 자동 시작 확인
- [x] `homemind-core` 실행 및 자동 시작 확인
- [x] MQTT TCP `1883` 리스너 확인
- [x] MQTT `1884` 리스너 확인
- [x] Flutter 웹 연결용 MQTT WebSocket 리스너 추가
- [x] SQLite DB 파일 확인
- [x] SQLite `PRAGMA integrity_check` 결과 `ok` 확인
- [x] 루트 저장공간 확인: 23%
- [x] 데이터 저장공간 확인: 1%
- [x] 저장공간 경고 기준 70% 확인
- [x] 자동 데이터 삭제 비활성화 확인

## 2. MQTT·JSON 계약

- [x] 스키마 버전 `pfn.v1` 확정
- [x] ACK 제한시간 3초 확정
- [x] 최대 명령 시도 3회 확정
- [x] 명령 유효시간 10초 확정
- [x] heartbeat 간격 5초 확정
- [x] 오프라인 판정시간 15초 확정
- [x] MQTT 토픽 목록 확정
- [x] telemetry JSON 스키마 작성
- [x] perception JSON 스키마 작성
- [x] task request/update JSON 스키마 작성
- [x] UI action JSON 스키마 작성
- [x] state request/response JSON 스키마 작성
- [x] 명령 및 ACK 중복 처리 규칙 확정
- [x] 공용 Pi·제어기·센서·액추에이터 ID 등록표 반영
- [x] UI 화면별 데이터·행동·MQTT 매핑 작성
- [x] command·ACK·heartbeat JSON 스키마 작성
- [x] P0 안전 경고 토픽·JSON·확인 규칙 확정
- [x] 개인정보·피드백·시스템 상태 토픽과 JSON 스키마 작성
- [x] 이동 조명·거실 조명·침대 커튼·FND 공식 ID 확정

## 3. Pi4 백엔드

- [x] `pfn/v1/#` MQTT 메시지 수신
- [x] 원본 MQTT JSON 보존
- [x] 잘못된 JSON 보존
- [x] `event_id` 기준 중복 방지
- [x] command와 ACK 저장
- [x] command와 ACK의 `trace_id` 일치 검사
- [x] telemetry 저장
- [x] heartbeat 저장
- [x] perception 저장
- [x] world state 갱신
- [x] task 제안과 상태 전환 저장
- [x] UI 승인 및 사용자 피드백 저장
- [x] 잘못된 Pi4 시간을 구분해 저장
- [x] boot ID와 uptime 저장
- [x] Pi4 heartbeat 5초 간격 발행
- [x] 저장공간 사용률 감시
- [x] 센서 종류별 정상 범위 규칙 확정 및 검사
- [x] heartbeat 중단 후 실제 15초 오프라인 시험
- [x] P0 안전 경고 처리 Pi4 자체시험
- [x] `resume`와 `expected_revision`·`action_token` 상태 전환 구현 및 시험
- [x] P0 안전·개인정보·피드백·system health 저장/발행 구현 및 시험
- [x] `target_device_id` 명령·ACK 검증, 저장 및 일치 검사
- [x] UI 동작을 실제 `target_device_id` 장치 명령으로 변환하는 시나리오 규칙 구현
- [x] Pi4 재부팅 후 서비스·DB 자동 복구 최종 시험
- [ ] 백엔드 상태 확인용 명령 정리

## 4. 랩탑 Flutter 프론트엔드

- [x] HomeMind 1024×600 기본 UI 작성
- [x] 가짜 데이터 시나리오 작성
- [x] 귀가·취침·야간 이탈·P0 버튼 작성
- [x] 연결 상태와 장치 상태 화면 작성
- [x] 작업 승인·거절 화면 작성
- [x] 프로그램 종료 버튼과 확인창 작성
- [x] 실제 MQTT 데이터 소스 작성
- [x] 가짜 데이터 모드와 실제 MQTT 모드 선택 기능
- [x] Pi4 연결 및 자동 재연결
- [x] Pi4 연결 끊김 표시
- [x] 실제 MQTT telemetry 센서값 실시간 표시 코드
- [x] UI Task 요청과 승인·거절 명령 발행
- [x] Task 상태와 ACK 성공·실패 결과에 따른 화면 갱신
- [x] 명령 시간 초과와 실패 이유 표시
- [ ] P0 안전 경고 확인 명령 발행
- [ ] SQLite 과거 데이터 조회 화면
- [x] 공용 ID 등록표 기반 Flutter 모델 변환 및 21개 ID 자동 일치 검사
- [x] 센서 출처 ID·마지막 수신 시각·15초 오프라인 화면
- [x] system health·SQLite 크기·저장공간 70% 경고 화면

## 5. 랩탑 가상 장치 및 통합시험

- [x] Pi4 실제 heartbeat를 랩탑에서 수신
- [x] 랩탑 → Pi4 브로커 → 랩탑 MQTT 왕복 시험
- [x] 공식 Pi4 자체시험 통과
- [x] 명령 중복 방지 시험
- [x] 명령 성공 및 ACK 저장 시험
- [x] telemetry·heartbeat 저장 시험
- [x] perception·world state 시험
- [x] task 승인과 사용자 피드백 시험
- [x] 잘못된 JSON 보존 시험
- [x] 랩탑 가짜 ESP32 센서값 연속 발행 도구 정리
- [x] 정상·범위 초과·중복·시간 오류 데이터 시험
- [x] heartbeat 중단 및 재연결 시험
- [ ] Flutter UI → Pi4 명령 → ACK → UI 갱신 전체 시험
- [ ] MQTT 연결 끊김과 자동 재연결 시험
- [ ] 전체 시나리오 통합시험
- [x] 계약 JSON·공용 ID·예제 메시지 정적 검증
- [x] 개별 장치 ID 명령 → 동일 장치 ID ACK 왕복 시험
- [x] Task 재개 및 오래된 revision 거절 시험
- [x] P0 확인·개인정보 모드·피드백·system health 통합시험

## 6. 안정성과 배포

- [x] Pi4 코어 IPK `0.5.0` 생성 및 실제 Pi4 업그레이드 설치
- [x] 전체 백엔드 자체시험 재실행
- [x] 로컬 임시 MQTT·SQLite 백엔드 통합시험
- [x] 기존 v3 DB → v4 무손실 마이그레이션 시험
- [ ] 장시간 가짜 데이터 시험 재실행
- [ ] 장시간 메모리·DB 증가 확인
- [ ] 저장공간 70% 경고 재시험
- [ ] Pi4 설치·삭제·재설치 시험
- [ ] Pi4 운영 명령 문서 최종 정리

## 2026-08-31 점검 결과

- Pi4 MAC: `dc:a6:32:85:f7:c8`
- MQTT와 백엔드 서비스: `active (running)`
- DB 무결성: `ok`
- 누적 MQTT 이벤트: 10,950건
- 누적 명령: 7건
- 누적 ACK: 14건
- 누적 telemetry: 7건
- 누적 heartbeat: 10,901건
- MQTT 왕복 결과: `MQTT_ROUNDTRIP_PASS`
- 전체 자체시험 결과: `SELFTEST_PASS`

## 2026-09-01 v0.4.2 설치 결과

- Docker Desktop 임시 소켓 오류 복구 및 ARM64 에뮬레이터 등록
- IPK: `dist-ipk/homemind-core_0.4.2_aarch64.ipk`
- SHA-256: `C12239015C5F96408094C054A427D25380A1981FD39CF81053B323F966CA72C9`
- 실제 Pi4 설치 버전: `0.4.2`
- Pi4 공식 ID와 heartbeat: `NODE-RPI4-01`, `pi4-core-0.4.2`
- 기존 DB v3 → v4 무손실 이전, 무결성: `ok`
- Pi4 실제 자체시험: `SELFTEST_PASS`
- 재부팅 후 MQTT·코어 자동 시작: `active`, `active`
- 재부팅 전 이벤트 11,986건 → 재부팅 후 11,994건으로 기존 데이터 유지
- 자동 삭제: 비활성화, 저장공간 경고: 70% 유지

## 2026-09-01 v0.5.0 1단계 완료 결과

- IPK: `dist-ipk/homemind-core_0.5.0_aarch64.ipk`
- SHA-256: `AACA56D1063344B23F5CC9993746ECAB0843EDDE77976CE462CB5D8774B0913B`
- 실제 Pi4 설치 버전: `0.5.0`
- 센서 범위·자료형·출처 검사와 잘못된 값 격리: 통과
- 가짜 ESP32 정상·혼합·오류 데이터 발행 도구: 완료
- heartbeat 중단 직후 online, 17초 후 offline: `OFFLINE_TEST_PASS`
- `prepare_home`, `prepare_sleep`, `night_bed_exit` 명령 규칙: 구현
- 시나리오 명령 → ACK → Task 완료 통합시험: `SELFTEST_PASS`
- 이동 조명·거실 조명·침대 커튼·FND 공식 ID: 등록 완료
- 재부팅 후 MQTT·Core 자동 시작: `active`, `active`
- 기존 DB 12,528개 MQTT 이벤트 유지, 무결성: `ok`
- 사용법: `docs/SENSOR_AND_SCENARIO_RULES_KO.md`

## 2026-09-01 Flutter 실제 MQTT 연결 결과

- Flutter 기본 실행을 가짜 데이터에서 Pi4 실제 MQTT 모드로 전환
- 웹: `ws://10.10.10.2:9001`, 네이티브: `10.10.10.2:1883`
- snapshot 5개 Task·6개 장치 노드 수신 성공
- 취침 Task 제안 → `proposed` → UI 거절 → `rejected` 왕복 성공
- 연결 끊김 표시와 3초 자동 재연결 구현
- Flutter 분석 오류 0개, 자동 테스트 5개 통과
- 실제 통신 시험: `MQTT_WEBSOCKET_UI_SMOKE_PASS`
- 다음: 가짜 ESP32를 함께 켜서 승인 → 실제 명령 → ACK → 완료 화면 전체 시험

## 2026-09-03 Flutter 노트북 단독 화면 완료 결과

- Pi4 없이 쓰는 `RUN_LAPTOP_PREVIEW.cmd` 추가
- ACK 시간초과 코드·원인·재시도 안내 카드 구현
- telemetry 센서값·출처 공식 ID·최종 수신 시각 표시 구현
- 15초 heartbeat 중단 시 ESP32 소속 장치 오프라인 표시 구현
- 등록표의 공식 장치 ID 21개를 Flutter 모델에 연결하고 자동 일치 검사 추가
- system health 구성요소, SQLite DB 크기, 저장공간 사용률과 70% 경고 화면 구현
- 프로그램 종료 버튼과 웹/네이티브 종료 안내 구현
- Flutter 분석 오류 0개, 자동 테스트 12개 통과
- 1024급 웹 미리보기에서 홈·실패·센서/기기·시스템 화면 직접 확인
- 실제 Pi4/ESP32 통신 시험은 장비를 다시 연결한 날 수행

## 2026-09-03 Pi4 재연결 및 자동 시작 설정 복구

- 유선 IP `10.10.10.2`, MAC `dc:a6:32:85:f7:c8`, SSH 접속 확인
- `dropbear.socket`, `homemind-mqtt`, `homemind-core` 모두 `enabled`·`active`
- MQTT TCP `1883`, 인증 포트 `1884`, Flutter WebSocket `9001` 리스너 확인
- Pi4 설치 버전 `homemind-core 0.5.0` 확인
- `/etc/systemd/system`의 구형 Core 서비스 파일을 `.legacy-20260903`으로 보존
- `/data/share/usr/lib/systemd/system/homemind-core.service`를 실제 실행 단위로 복구
- Core 실행 ID를 `NODE-RPI4-01`로 복구하고 센서·시나리오 규칙 경로 적용
- 루트 파일시스템은 작업 후 다시 읽기 전용으로 복구
- SQLite 무결성 `ok`, 기존 MQTT 이벤트 13,691건 보존 확인
- 실제 WebSocket snapshot 및 Task 제안→거절 왕복 `MQTT_WEBSOCKET_UI_SMOKE_PASS`
- 왕복 시험 후 SQLite 무결성 `ok`, MQTT 이벤트 13,732건 확인
- 인터넷 없는 재부팅 시간은 invalid로 판정되어 잘못된 시간 보호 로직이 동작 중
