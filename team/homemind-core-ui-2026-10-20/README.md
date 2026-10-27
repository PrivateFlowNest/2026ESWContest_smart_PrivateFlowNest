# HomeMind Pi4 TaskFlow Core

> **2026-10-21 변경**: `contracts/`는 저장소 루트 `contracts/pfn.v1/`로 옮겼다(정본 단일화). 이 문서의 `contracts/...` 경로는 그 위치로 읽는다. 검증: `python tests/validate-contracts.py`(루트 검증기 래퍼) 또는 `python contracts/validate-contracts.py`. Docker 빌드는 `--build-context contracts=../../contracts/pfn.v1`(scripts/build*.ps1에 반영).

Pi4-only C++ implementation of the HomeMind state and TaskFlow backend. The
target is the Atlas aarch64 image used by the contest Raspberry Pi 4.

## Responsibilities

- Subscribe to every `pfn/v1/#` MQTT message.
- Preserve the original JSON before extracting fields.
- Store commands, acknowledgements, telemetry, and heartbeats in SQLite.
- Deduplicate QoS redelivery by `event_id` without deduplicating a device's
  sequence number across reboots.
- Update command state only when both `command_id` and `trace_id` match.
- Retry an unanswered command at three-second intervals with the same command ID,
  then fail its Task with `ACK_TIMEOUT` after the third attempt.
- Use boot uptime for ACK retry deadlines so a rebooted or corrected wall clock
  cannot leave a pending command stuck forever.
- Track the latest device state and mark stale devices offline.
- Publish a Pi4 heartbeat every five seconds.
- Check database and filesystem capacity every minute and log a warning at 70%.
- Preserve the Linux boot ID, boot-relative receive time, and receiver clock
  validity so offline clock resets never make uncertain timestamps look trusted.
- Include clock trust and boot uptime in UI snapshots; use boot uptime for
  offline and periodic storage checks.
- Store Pi5 perception results as evidence and maintain the latest world state.
- Accept Pi5 task proposals and manage their state and revision in SQLite.
- Apply UI approve, reject, and cancel actions and preserve user feedback.
- Return one consistent snapshot of tasks, devices, and world state to the Pi5 UI.

Automatic data deletion is explicitly disabled. The capacity monitor only reads
sizes and updates the single `storage_status` row.

Mutable target files live below `/data/share/homemind`. The Atlas root
filesystem is not used for the database.

## Build

Run `scripts/build.ps1` on Windows. It builds for `linux/arm64` in Docker and
places the deployable files in `dist/`.

## TaskFlow message path

1. Pi5 publishes perception to `pfn/v1/ai/perception`.
2. Pi5 proposes a task on `pfn/v1/core/task/request`.
3. Pi4 stores it as `proposed` and publishes `pfn/v1/core/task/update`.
4. The UI publishes approve, reject, or cancel on `pfn/v1/ui/action`.
5. Pi4 records the transition and publishes the new task revision.
6. The UI requests a snapshot on `pfn/v1/core/state/request`; Pi4 replies on
   `pfn/v1/core/state/response/{client_id}`.

All TaskFlow messages use QoS 1 and retained `false`. The exact JSON contracts
are in `contracts/`.

Shared physical IDs are defined in `contracts/device-registry.json`. UI screen
requirements and their MQTT/JSON mappings are in `docs/UI_BACKEND_CONTRACT_KO.md`.
Run `python tests/validate-contracts.py` after changing a contract or an ID.

## Target paths

- Programs: `/data/share/homemind/bin/`
- Extra runtime libraries: `/data/share/homemind/lib/`
- Database: `/data/share/homemind/db/homemind.db`
- Configuration: `/data/share/homemind/config/`
- Service: `/etc/systemd/system/homemind-core.service`

## Database safety

SQLite runs with WAL journaling, foreign keys, a busy timeout, and transactional
writes. Every message is first represented in `mqtt_events`; invalid or unknown
messages remain inspectable instead of being silently discarded.

Sensor data is still stored when the Pi4 clock is unverified. Such rows are
marked with `first_receiver_time_valid=0` and can later be related to a trusted
time using their `receiver_boot_id` and `first_received_uptime_ms`.
