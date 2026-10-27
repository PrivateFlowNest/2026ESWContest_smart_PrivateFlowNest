# 진행 기록

## 00:00 기준 상태

Pi4와 세 개 통신 포트가 정상이며 운영 DB 무결성이 확인되었다. 기존 데이터와 설정은 변경하지 않았다.

## 전체 경로 1차 시험

- `prepare_home`, `prepare_sleep`, `night_bed_exit` 세 시나리오 완료
- 가상 `CTRL-ESP32-ACT-01`, `CTRL-ESP8266-LED-01`의 telemetry 수신
- 두 제어기의 heartbeat와 ACK 수신
- Pi4 snapshot 수신: 작업 11개, 노드 상태 8개
- 결과: `PI4_FULL_PATH_SMOKE_PASS`

## 센서·오프라인 시험

- 정상 센서값 4개 저장
- 범위 초과·잘못된 자료형·미등록 센서값 12개 거부
- 공식 노드 `CTRL-OCTOPUS-01`이 heartbeat 중단 17초 뒤 offline으로 전환

## 명령 시간초과 수정

- 발견 문제: ACK가 없는 시나리오가 계속 running으로 남았음
- 수정: 3초 간격, 같은 command ID, attempt 1·2·3으로 재전송
- 세 번째 시도에도 ACK가 없으면 Task를 failed로 전환
- Flutter용 `ACK_TIMEOUT`, 설명, 재시도 허용 여부를 응답에 포함
- 노트북 별도 DB 및 Pi4 별도 DB 회귀시험 통과
- 운영 실행 파일 교체 후 실제 Pi4 시험 통과
- 이전 실행 파일은 `/data/share/homemind/rollback-11h-20260903`에 보존

## MQTT 복구·저장공간 경고

- 운영 MQTT를 잠시 중단하고 재시작
- Flutter 데이터 연결이 끊김을 감지한 뒤 자동 재연결: 통과
- MQTT와 Core 서비스 active/enabled 유지
- 별도 DB에서 경고 기준을 1%로 낮춘 안전 시험: 경고 발생
- 디스크를 채우지 않았고 automatic_delete=false 확인

## SQLite 작업 기록 화면

- Pi4 snapshot이 SQLite에서 전달하는 최근 작업 최대 100개를 별도 화면에 표시
- 완료·실패 개수, 상태, 오류 코드, revision, 갱신 시각 표시
- Flutter 분석 0건, 자동 테스트 12건 통과(실장비 전용 시험 1건은 기본 실행에서 제외)

## 시간 오류 표시 보완

- Pi4가 현재 시간의 신뢰 여부와 부팅 후 경과 시간을 state snapshot에 포함하도록 보완
- 시간이 신뢰되지 않으면 기록 화면과 시스템 화면에 경고 표시
- 작업 기록과 시스템 상태의 신뢰할 수 없는 날짜 대신 `시간 미확인`으로 표시하며 원본 데이터는 보존
- JSON 규격 검사, 노트북 Core selftest, Flutter 분석과 12개 자동 테스트 통과
- 최신 ARM64 실행 파일을 Pi4의 별도 MQTT 포트·별도 DB에서 시험: 27개 검사 및 DB 무결성 통과
- 잘못된 벽시계가 저장공간 점검 주기를 멈추지 않도록 부팅 후 경과시간 기준으로 변경
- 공식 장치 ID 21개가 Flutter 목록과 일치하는지 확인
- 구버전 DB를 schema version 4로 올리는 마이그레이션 회귀시험 통과
- 최신 ARM64 Core의 ACK 미수신 시험 재실행: 동일 command ID로 attempt 1·2·3 후 `ACK_TIMEOUT` 전환 통과
- 시험용 MQTT 포트와 Core 프로세스는 종료 후 정리했으며 운영 서비스와 장시간 시험은 계속 정상 동작
- Task 상세에서도 시간이 신뢰되지 않을 때 실패 발생 시각을 `시간 미확인`으로 표시
- 개발 미리보기의 이전 웹 캐시를 비활성화해 새 빌드가 곧바로 표시되도록 실행 파일 보완
- 프로그램 종료 버튼의 확인 창을 자동 테스트와 실제 웹 화면에서 확인
- `scripts/status.ps1`이 저장된 host key와 5초 제한을 사용하도록 보완하고 실제 Pi4 상태 조회 통과
- Windows PowerShell 5에서도 동작하도록 원격 명령 전달 방식을 수정하고 `CHECK_PI4_STATUS.cmd` 더블클릭 시험 통과
- fake ESP32, selftest, 오프라인 시험 스크립트도 저장된 Pi4 host key와 5초 연결 제한을 사용하도록 통일
- 오프라인 시험 ID를 공식 등록표의 `CTRL-OCTOPUS-01`로 통일하고 PowerShell 문법 검사 통과

## 격리 장시간 시험 진행 중

- 운영 MQTT·DB와 분리된 포트 28883 및 별도 DB 사용
- 200ms 간격 가짜 센서값 발행
- 8분 기준 발행 2,401건 / 저장 2,400건, Core RSS 약 9.6MiB
- 자동 삭제와 실제 디스크 채우기 없음
- 시간·저장공간 보완까지 포함한 최종 ARM64 실행 파일도 포트 28885와 별도 DB에서 동일한 2시간 시험을 추가 시작
- 두 시험의 운영 DB·운영 MQTT와의 경로 분리를 확인
- 첫 번째 2시간 시험 완료: 7,203초, telemetry 36,000/36,000건, 저장률 100%, 발행 실패 0건
- 첫 번째 시험 Core PID 유지, warm RSS 7,952KiB → 최종 9,616KiB(증가 1,664KiB), DB 49,352,760 bytes, integrity `ok`
- 첫 번째 시험 포트 28883과 Core·broker·soaktest 프로세스 자동 정리 확인
- 두 번째 2시간 시험 완료: 7,203초, telemetry 36,000/36,000건, 저장률 100%, 발행 실패 0건
- 두 번째 시험 Core PID 유지, warm RSS 7,844KiB → 최종 9,508KiB(증가 1,664KiB), DB 49,373,192 bytes, integrity `ok`
- 두 번째 시험 포트 28885와 Core·broker·soaktest 프로세스 자동 정리 확인
- 0.5.2 2시간 시험 완료: 7,203초, telemetry 36,000/36,000건, 저장률 100%, 발행 실패 0건
- 0.5.2 시험 Core PID 유지, warm RSS 7,956KiB → 최종 9,620KiB(증가 1,664KiB), DB 49,566,016 bytes, integrity `ok`
- 0.5.2 시험 포트 28887과 Core·broker·soaktest 프로세스 자동 정리 확인

## 재설치 패키지

- 최종 버전: `homemind-core 0.5.1`
- 파일: `dist-ipk/homemind-core_0.5.1_aarch64.ipk`
- ARM64 형식, Core/selftest 바이너리 일치, 자동 시작 링크 3개 포함 여부 확인
- SHA-256: `b1afb57ea1904b0be4375e13505d27a12b9f694046980f85b3fd79ee02bfa0c0`
- 현재 Pi4에는 아직 IPK 자체를 재설치하지 않고, 검증한 동일 Core 바이너리만 안전 배포할 예정
- IPK의 `control.tar.gz`와 `data.tar.gz`를 다시 열어 필수 실행 파일·설정·systemd 서비스·자동 시작 링크가 모두 포함된 것을 재확인
- 시간 점프 보완 뒤 최종 후보를 `homemind-core 0.5.2`로 올림
- 파일: `dist-ipk/homemind-core_0.5.2_aarch64.ipk`
- 최종 Core SHA-256: `877f37df26bebe442d20552739426f7c044533dd27d356718b3b3978fa704e81`
- 최종 IPK SHA-256: `13aeca1b2747aa7c0896740c5b82db7fc6a0289b8fa854ad2e66f82c30f5c6c8`
- 0.5.2 IPK의 Core·selftest·soaktest·simulator를 모두 현재 ARM64 빌드와 일치시킴
- 장치 등록표·센서 규칙·시나리오 규칙 3개를 IPK 설정 보존 목록에 추가해 업그레이드 시 사용자 설정을 보호
- 최종 IPK를 Pi4 시험 폴더로 전송한 뒤 Pi4의 실제 `opkg 0.6.3`으로 설치 없이 메타데이터를 읽고, AArch64 0.5.2 및 전송 전후 SHA-256 일치를 확인

## 재부팅 시계 점프 보완

- 추가 발견: 재부팅 뒤 벽시계가 과거로 돌아가면 pending 명령의 기존 wall-clock ACK 기한이 멈출 수 있었음
- 수정: ACK 재시도·최종 실패 판정 기한을 메모리의 부팅 후 경과시간으로 계산
- 벽시계가 신뢰될 때만 메시지의 절대 만료 시각을 보조 조건으로 적용
- DB의 pending 명령에 2100년 미래 시각을 강제 주입한 뒤에도 같은 command ID로 attempt 1·2·3, `ACK_TIMEOUT` 전환 통과
- 최신 heartbeat·시스템 상태·작업 조회가 재부팅 전의 큰 wall-clock 값에 가려지지 않도록 SQLite 입력 순서를 우선하도록 보완
- 실제 MQTT로 2100년 시각의 degraded 상태를 먼저 보내고 1970년 시각의 healthy 상태를 나중에 보낸 뒤 snapshot이 나중 입력을 선택: `LATEST_INPUT_ORDER_PASS`
- 최종 ARM64 0.5.2 실행 파일의 27개 selftest와 DB 무결성 검사 통과
- 0.5.2 전용 2시간 고속 센서 시험을 별도 포트 28887·별도 DB에서 시작
- 노트북에서 AddressSanitizer·UndefinedBehaviorSanitizer를 켠 0.5.2 Core와 27개 selftest 통과, 메모리 오류·정의되지 않은 동작 발견 없음

## 최종 노트북 회귀시험

- 계약 JSON 19개 스키마, 예제 5개, 공식 장치 21개 검사 통과
- DB schema version 4와 C++ 내장 스키마 일치 검사 통과
- Flutter 정적 분석 오류 0개
- Flutter 자동시험 12개 통과(실장비 전용 1개는 기본 실행에서 제외)
- 웹 release 빌드 재생성 성공

## 운영 배포·재부팅·최종 수정

- 최종 Core `877f37df...704e81`을 Pi4 운영 경로에 안전 배포하고 이전 파일을 rollback 폴더에 보존
- 첫 재부팅에서 `/etc/systemd/system/homemind-core.service`의 오래된 연결 문제를 발견
- 실제 서비스 파일을 `/etc/systemd/system`에 설치하고 루트 파일시스템을 다시 읽기 전용으로 복구
- 두 번째 실제 재부팅 뒤 Dropbear·유선망·MQTT·Core 모두 자동 실행, Core 재시작 0회
- 부팅 ID `211ac962-6ddb-4e9f-a916-c29f77b549b5`, DB 무결성 `ok`, 자동 삭제 `false`
- 재부팅 후 Flutter 시나리오 3종과 가상 ESP32/ESP8266 ACK 전체 경로 재통과
- 켜져 있는 UI의 상태조회 응답과 자체검사 응답이 섞이는 문제를 `request_id` 필터로 수정
- 수정된 ARM64 자체검사·ASan·UBSan 통과
- `FAN-01`의 공식 위치와 Flutter 표시를 화장실 환기 팬으로 통일
- 최종 계약검사, DB schema 검사, Flutter 분석·12개 시험·Web release 빌드 재통과
- 최종 IPK 내용물의 Core·selftest 해시, 자동 시작 설치 코드, 화장실 zone, SHA-256 검증 통과
