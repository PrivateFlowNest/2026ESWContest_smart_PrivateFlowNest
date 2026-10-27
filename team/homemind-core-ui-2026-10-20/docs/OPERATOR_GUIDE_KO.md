# HomeMind Pi4 사용 설명서

## 평소 전원 켜기

1. Pi4 전원과 PC–Pi4 랜선을 연결한다.
2. 약 30초 기다린다.
3. PC에서 `ping 10.10.10.2`로 응답을 확인한다.

MQTT와 SQLite 코어는 사람이 명령을 입력하지 않아도 자동으로 시작된다.

## SSH 접속

일반 PowerShell에서는 다음 명령을 사용한다.

```powershell
ssh root@10.10.10.2
```

`ssh`를 찾을 수 없다는 오류가 나오면 다음 명령을 사용한다.

```powershell
& "$env:windir\Sysnative\OpenSSH\ssh.exe" root@10.10.10.2
```

## 정상 상태 확인

Pi4에 접속한 뒤 다음 세 줄을 확인한다.

```sh
systemctl is-active homemind-ethernet
systemctl is-active homemind-mqtt
systemctl is-active homemind-core
```

세 결과가 모두 `active`이면 정상이다.

PC에서 프로젝트 폴더를 연 상태라면 다음 파일을 실행해 서비스, DB, 용량을
한꺼번에 확인할 수 있다.

```powershell
& .\scripts\status.ps1
```

출력의 `Storage monitor` 부분을 확인한다.

```text
automatic_delete=false
database=0.25 MiB
storage_used=1.2% / warning_at=70%
OK: storage usage is below 70%
```

- `automatic_delete=false`: 자동 삭제가 꺼져 있다는 뜻이다.
- `database`: SQLite 본체와 WAL을 합친 현재 크기다.
- `storage_used`: DB가 저장된 Pi4 공간의 전체 사용률이다.
- 사용률이 70% 이상이면 `WARNING`으로 바뀐다.

Pi4는 이 값을 1분마다 자동 확인한다. 경고 로그는 다음 명령으로 확인한다.

```sh
journalctl -u homemind-core --no-pager | grep STORAGE_
```

`STORAGE_WARNING`이 나와도 프로그램이 데이터를 자동 삭제하지 않는다. 새로운
데이터를 다른 곳에 옮기거나 보관 정책을 정하기 전까지는 사용자가 직접 DB를
삭제하지 않는다.

자세한 상태는 다음과 같이 확인한다.

노트북에서는 프로젝트 폴더의 `CHECK_PI4_STATUS.cmd`를 더블클릭하면 서비스,
설치된 Core 해시와 보고 버전, DB 무결성, 저장공간, 시간 신뢰 여부를 한 번에
확인할 수 있다.

```sh
systemctl status homemind-core --no-pager
journalctl -u homemind-core --no-pager -n 30
```

## 인터넷이 없을 때 시간 보호

Pi4에는 전원이 꺼져도 계속 가는 시계가 없으므로, 인터넷이나 별도의 RTC가 없으면 실제
시각을 정확히 알 수 없다. HomeMind는 이때에도 센서 데이터를 버리지 않는다.

새 MQTT 원본 기록에는 다음 수신 정보가 함께 저장된다.

- `receiver_boot_id`: 데이터를 받은 Pi4 부팅을 구별하는 ID
- `first_received_uptime_ms`: 해당 부팅 후 데이터를 처음 받은 경과시간
- `first_receiver_time_valid`: 수신 당시 Pi4 시계 동기화 여부 (`1` 정상, `0` 미확인)

같은 `event_id`가 다시 들어오면 마지막 수신 시각 정보도 별도로 갱신된다. 기존 데이터는
삭제하거나 임의로 시간을 고치지 않는다. 나중에 정확한 시간을 받은 뒤 `boot_id`와
`uptime`을 이용해 미확인 기록을 구별하고, 같은 부팅 안에서 받은 기록의 시각을 보정할
수 있다. Pi4 전원이 꺼져 있던 구간의 정확한 길이는 RTC나 외부 시간 제공자가 없으면
계산할 수 없다.

Windows에서 상태 확인 도구를 실행하면 현재 결과가 `time=VALID` 또는
`time=UNVERIFIED (data is still saved)`로 표시된다.

Flutter는 Pi4 snapshot의 `clock.valid`를 확인한다. 값이 `false`이면 작업 기록과
시스템 상태의 날짜를 `시간 미확인`으로 표시한다. 장치 오프라인 15초 판정과 저장공간
1분 점검, ACK 3초 재시도는 실제 날짜가 아니라 부팅 후 경과시간을 사용하므로 시계가
틀려도 멈추지 않는다.

`UNVERIFIED`는 프로그램 오류가 아니다. 실제 시각을 확신할 수 없다는 뜻이며 데이터는
계속 SQLite에 저장된다. 항상 정확한 시간이 필요하면 RTC 모듈, 인터넷 시간 동기화,
또는 Pi5/PC의 시간 전달 중 하나가 필요하다.

## Pi4 단독 전체 시험

ESP32와 Pi5가 없어도 다음 명령으로 전체 흐름을 시험할 수 있다.

```sh
LD_LIBRARY_PATH=/data/share/homemind/lib \
  /data/share/homemind/bin/homemind-selftest \
  --broker-host 10.10.10.2 \
  --broker-port 1883 \
  --database /data/share/homemind/db/homemind.db \
  --node-id CTRL-ESP32-ACT-01
```

마지막에 `SELFTEST_PASS`가 나오면 MQTT, 명령, ACK, 중복 방지,
telemetry, heartbeat, Pi5 작업 제안, UI 승인, 상태 응답, SQLite가 모두 정상이다.

## 최근 저장 내용 확인

```sh
sqlite3 /data/share/homemind/db/homemind.db
```

SQLite 화면에서 다음 예시를 사용할 수 있다.

```sql
.headers on
.mode column
SELECT node_id, status, last_seen_at_ms FROM devices;
SELECT command_id, target_node_id, status FROM commands ORDER BY rowid DESC LIMIT 10;
SELECT command_id, ok, duplicate, status FROM acknowledgements ORDER BY rowid DESC LIMIT 10;
SELECT node_id, seq, simulated_value FROM telemetry ORDER BY rowid DESC LIMIT 10;
PRAGMA integrity_check;
.quit
```

`PRAGMA integrity_check;` 결과가 `ok`이면 DB 파일이 정상이다.

## 문제 발생 시

코어만 다시 시작한다.

```sh
systemctl restart homemind-core
```

MQTT까지 다시 시작한다.

```sh
systemctl restart homemind-mqtt
systemctl restart homemind-core
```

서비스가 자동으로 다시 연결되므로 DB 파일을 삭제하거나 초기화하지 않는다.

## 안전하게 종료

Pi4에 접속한 상태에서 다음 명령을 사용한다.

```sh
systemctl poweroff
```

SSH 연결이 끊어진 뒤 Pi4의 동작 LED가 멈출 때까지 기다린 후 전원을 분리한다.
