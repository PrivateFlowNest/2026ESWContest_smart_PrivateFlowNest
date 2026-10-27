import 'models.dart';

class OfficialDevice {
  const OfficialDevice(
      {required this.id,
      required this.name,
      required this.shortName,
      required this.location,
      required this.controllerNodeId,
      required this.category,
      required this.hardware,
      this.pin = '',
      this.wiring = WiringStatus.wired,
      this.openDegrees,
      this.closedDegrees,
      this.initialValue = 'OFF',
      this.shown = true});
  final String id;

  /// Full display name for the device list ("TV 표시등").
  final String name;

  /// Name used in sentences ("TV 음소거", "에어컨을 켤까요?").
  final String shortName;
  final String location;
  final String controllerNodeId;
  final String category;
  final String hardware;
  final String pin;
  final WiringStatus wiring;

  /// Registry `positions.open` / `positions.closed` (servos only). Registry v4: codes for 열림/닫힘 of the
  /// open/close motors, not angles (the ESP32 turns them into motor commands).
  final int? openDegrees;
  final int? closedDegrees;
  final String initialValue;

  /// false: the ID stays in the registry (and here, for the catalog check) but the device list does not show
  /// it: devices not in the model (registry assignment_status removed_from_model, 2026-10-25) and the doll axis
  /// (person role, not a home device; 2026-10-22).
  final bool shown;
}

/// contracts/pfn.v1/device-registry.json registry_version 4와 같은 공식 장치 ID.
/// `tool/check_device_catalog.py`가 ID·담당 제어기·wiring_status를 비교한다.
const officialDevices = <OfficialDevice>[
  OfficialDevice(
      id: 'SERVO-DOOR-01',
      name: '현관문',
      shortName: '현관문',
      location: '현관',
      controllerNodeId: 'CTRL-ESP32-ACT-01',
      category: '서보',
      hardware: '열기/닫기 모터',
      pin: 'GPIO19',
      openDegrees: 90,
      closedDegrees: 0,
      initialValue: '닫힘'),
  OfficialDevice(
      id: 'SERVO-DOOR-02',
      name: '화장실 문',
      shortName: '화장실 문',
      location: '화장실',
      controllerNodeId: 'CTRL-ESP32-ACT-01',
      category: '서보',
      hardware: 'MG90S',
      wiring: WiringStatus.notWired,
      initialValue: '철거',
      shown: false),
  OfficialDevice(
      id: 'SERVO-WINDOW-01',
      name: '창문',
      shortName: '창문',
      location: '거실',
      controllerNodeId: 'CTRL-ESP32-ACT-01',
      category: '서보',
      hardware: '열기/닫기 모터',
      pin: 'GPIO21',
      openDegrees: 90,
      closedDegrees: 0,
      initialValue: '닫힘'),
  OfficialDevice(
      id: 'FAN-01',
      name: '화장실 환기 팬',
      shortName: '환기 팬',
      location: '화장실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: '팬 (켜기/끄기)',
      hardware: '12V 4010 팬',
      pin: 'D2'),
  OfficialDevice(
      id: 'LED-WASHER-01',
      name: '세탁기 표시등',
      shortName: '세탁기',
      location: '거실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'LED',
      hardware: 'LED (빨강)',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'LED-AIRCON-01',
      name: '에어컨 표시등',
      shortName: '에어컨',
      location: '거실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'LED',
      hardware: 'LED (빨강)',
      pin: 'D1'),
  OfficialDevice(
      id: 'LED-TV-01',
      name: 'TV 표시등',
      shortName: 'TV',
      location: '거실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'LED',
      hardware: 'LED (빨강)',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'LED-LAPTOP-01',
      name: '노트북 표시등',
      shortName: '노트북',
      location: '책상',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'LED',
      hardware: 'LED (빨강)',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'LED-LIVING-01',
      name: '거실 조명',
      shortName: '거실 조명',
      location: '거실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'LED',
      hardware: 'LED (빨강)',
      pin: 'D0'),
  OfficialDevice(
      id: 'LED-GUIDE-01',
      name: '야간 이동 조명',
      shortName: '야간 이동 조명',
      location: '거실→화장실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'LED',
      hardware: 'LED',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'SERVO-BED-CURTAIN-01',
      name: '침대 커튼',
      shortName: '침대 커튼',
      location: '침대',
      controllerNodeId: 'CTRL-ESP32-ACT-01',
      category: '서보',
      hardware: 'MG90S',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'FND-WASHER-01',
      name: '세탁기 소음 표시',
      shortName: '세탁기 표시',
      location: '거실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'FND',
      hardware: 'FND',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'FND-AIRCON-01',
      name: '에어컨 소음 표시',
      shortName: '에어컨 표시',
      location: '거실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'FND',
      hardware: 'FND',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'FND-TV-01',
      name: 'TV 소음 표시',
      shortName: 'TV 표시',
      location: '거실',
      controllerNodeId: 'CTRL-ESP8266-LED-01',
      category: 'FND',
      hardware: 'FND',
      wiring: WiringStatus.notWired,
      initialValue: '모형에 없음',
      shown: false),
  OfficialDevice(
      id: 'MIC-INDOOR-01',
      name: '실내 마이크',
      shortName: '마이크',
      location: '거실',
      controllerNodeId: 'NODE-RPI5-02',
      category: '센서',
      hardware: 'USB 마이크',
      pin: 'USB',
      initialValue: '-- dB'),
  OfficialDevice(
      id: 'CAM-INDOOR-01',
      name: '실내 카메라',
      shortName: '실내 카메라',
      location: '거실',
      controllerNodeId: 'NODE-RPI5-02',
      category: '카메라',
      hardware: 'USB 웹캠',
      pin: 'USB3',
      initialValue: '대기'),
  OfficialDevice(
      id: 'CAM-OUTDOOR-01',
      name: '현관 카메라',
      shortName: '현관 카메라',
      location: '현관 밖',
      controllerNodeId: 'NODE-RPI5-02',
      category: '카메라',
      hardware: 'USB 웹캠',
      pin: 'USB',
      initialValue: '대기'),
  OfficialDevice(
      id: 'MOTOR-XY-X-01',
      name: 'X축 이동 모터',
      shortName: 'X축 모터',
      location: '거실',
      controllerNodeId: 'CTRL-OCTOPUS-01',
      category: '모터',
      hardware: 'NEMA17',
      pin: 'MOTOR0',
      initialValue: 'STOP',
      shown: false),
  OfficialDevice(
      id: 'MOTOR-XY-Y-01',
      name: 'Y축 이동 모터',
      shortName: 'Y축 모터',
      location: '거실',
      controllerNodeId: 'CTRL-OCTOPUS-01',
      category: '모터',
      hardware: 'NEMA17',
      pin: 'MOTOR1',
      initialValue: 'STOP',
      shown: false),
  OfficialDevice(
      id: 'SENSOR-HOME-X-01',
      name: 'X축 원점 센서',
      shortName: 'X축 원점 센서',
      location: '거실',
      controllerNodeId: 'CTRL-OCTOPUS-01',
      category: '센서',
      hardware: '광센서',
      pin: 'J27 PG6',
      shown: false),
  OfficialDevice(
      id: 'SENSOR-HOME-Y-01',
      name: 'Y축 원점 센서',
      shortName: 'Y축 원점 센서',
      location: '거실',
      controllerNodeId: 'CTRL-OCTOPUS-01',
      category: '센서',
      hardware: '광센서',
      pin: 'J29 PG9',
      shown: false),
];

OfficialDevice? officialDeviceById(String id) {
  for (final device in officialDevices) {
    if (device.id == id) return device;
  }
  return null;
}

List<DeviceStatus> buildOfficialDeviceStatuses({required bool online}) {
  final now = DateTime.now();
  return officialDevices
      .where((device) => device.shown)
      .map((device) => DeviceStatus(
            id: device.id,
            name: device.name,
            location: device.location,
            online: online,
            active: false,
            displayValue: device.initialValue,
            controllerNodeId: device.controllerNodeId,
            category: device.category,
            lastSeenAt: online ? now : null,
            wiringStatus: device.wiring,
            hardware: device.hardware,
            pin: device.pin,
          ))
      .toList(growable: false);
}
