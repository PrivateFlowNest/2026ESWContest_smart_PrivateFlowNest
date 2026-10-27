# HomeMind UI–Pi4 통합 확인 결과

확인일: 2026-09-04  
구성: Windows 랩탑 + Raspberry Pi 4 (`10.10.10.2`)  
Docker: 사용하지 않음

## 완료 결과

| 항목 | 결과 | 확인 내용 |
| --- | --- | --- |
| UI 전체 점검 | 통과 | 1024×600 기준 홈, Task, Task 상세, 개인정보 화면 잘림 없음 |
| Task 양방향 통신 | 통과 | 귀가, 취침 준비, 야간 침대 이탈 시나리오와 장치 ACK 확인 |
| 개인정보 연동 | 통과 | UI에서 `일반 → 카메라 차단 → 일반` 변경 후 Pi4 확정 상태 재수신 |
| 피드백 저장 | 통과 | `action_feedback / positive` 이벤트가 Pi4 SQLite에 저장됨 |
| Task 상세 필드 | 통과 | intent, 대상 장치, 신뢰도, revision, ACK 결과 표시 |
| P0 안전 경고 | 통과 | 긴급 화면 표시, ACK 전송, 활성 경고 0건으로 해제 확인 |
| 장치 오프라인 | 통과 | 가짜 ESP32 중단 17초 뒤 `offline` 판정 |
| MQTT 재연결 | 통과 | 브로커를 5초 중단한 뒤 UI 자동 재연결 |
| DB 무결성 | 통과 | SQLite `integrity_check = ok` |
| 자동 검사 | 통과 | 정적 분석 오류 0개, 테스트 16개 통과, 실장비 전용 1개 보류 |
| 웹 실행본 | 통과 | Flutter release 빌드 성공 |

## 최종 Pi4 상태

- `homemind-core.service`: active
- `homemind-mqtt.service`: active
- `dropbear.socket`: active
- 개인정보: `normal`, 카메라 허용, 마이크 허용, 녹화 안 함, 로컬 처리
- 확인하지 않은 P0 경고: 0건
- 자동 삭제: 비활성화 유지
- 저장공간 경고 기준: 70% 유지

## 확인된 실제 통합 데이터

- 완료 시나리오: `prepare_home`, `prepare_sleep`, `night_bed_exit`
- ACK 장치: `CTRL-ESP32-ACT-01`, `CTRL-ESP8266-LED-01`
- 피드백 이벤트: `2f8eea5b-4587-4d76-beba-b1e908611aca`
- Pi4 시계 상태: 미확인. 잘못된 날짜를 정상 시각으로 취급하지 않는 보호 로직이 동작함.

## 현재 장비로 남은 선택 작업

- 몇 시간짜리 장시간 부하 시험
- 오래된 자체시험 Task가 많이 보이는 목록의 표시 정리
- 재설치용 IPK를 깨끗한 환경에 실제 복원하는 연습

위 항목은 현재 기능 완성에 필수는 아니다. 실제 카메라 인식, 물리 센서·액추에이터,
Pi5용 Atlas 빌드·터치·자동 실행은 해당 장비가 있을 때 최종 확인한다.
