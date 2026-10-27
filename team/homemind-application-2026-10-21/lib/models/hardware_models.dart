class HardwareDevice {
  const HardwareDevice({
    required this.id,
    required this.name,
    required this.hardware,
    required this.controllerName,
    required this.category,
    required this.online,
    required this.state,
    required this.supportedCommands,
    this.controllerId,
    this.alias,
    this.pendingCommand,
    this.lastTaskState,
    this.lastFailureCode,
  });

  factory HardwareDevice.fromJson(Map<String, dynamic> json) => HardwareDevice(
    id: json['id'] as String? ?? '',
    name: json['name'] as String? ?? '알 수 없는 장치',
    hardware: json['hardware'] as String? ?? '',
    controllerId: json['controller_id'] as String?,
    controllerName: json['controller_name'] as String? ?? '',
    alias: json['alias'] as String?,
    category: json['category'] as String? ?? 'sensor',
    online: json['online'] as bool? ?? false,
    state: json['state'] as String? ?? 'UNKNOWN',
    supportedCommands: ((json['supported_commands'] as List?) ?? const [])
        .whereType<String>()
        .toList(),
    pendingCommand: json['pending_command'] as String?,
    lastTaskState: json['last_task_state'] as String?,
    lastFailureCode: json['last_failure_code'] as String?,
  );

  final String id;
  final String name;
  final String hardware;
  final String? controllerId;
  final String controllerName;
  final String? alias;
  final String category;
  final bool online;
  final String state;
  final List<String> supportedCommands;

  /// Command sent to core and not finished yet (state changes only after core
  /// reports the task completed, i.e. after the ESP ACK).
  final String? pendingCommand;
  final String? lastTaskState;
  final String? lastFailureCode;

  bool get controllable => supportedCommands.isNotEmpty;
  bool get pending => pendingCommand != null;

  HardwareDevice copyWith({bool? online, String? state}) => HardwareDevice(
    id: id,
    name: name,
    hardware: hardware,
    controllerId: controllerId,
    controllerName: controllerName,
    alias: alias,
    category: category,
    online: online ?? this.online,
    state: state ?? this.state,
    supportedCommands: supportedCommands,
    pendingCommand: pendingCommand,
    lastTaskState: lastTaskState,
    lastFailureCode: lastFailureCode,
  );
}

class HardwareSnapshot {
  const HardwareSnapshot({
    required this.connected,
    required this.devices,
    required this.updatedAt,
  });

  factory HardwareSnapshot.fromJson(Map<String, dynamic> json) =>
      HardwareSnapshot(
        connected: json['connected'] as bool? ?? false,
        devices: ((json['devices'] as List?) ?? const [])
            .whereType<Map>()
            .map(
              (item) => HardwareDevice.fromJson(item.cast<String, dynamic>()),
            )
            .toList(),
        updatedAt: json['updated_at'] as String? ?? '',
      );

  final bool connected;
  final List<HardwareDevice> devices;
  final String updatedAt;

  HardwareSnapshot copyWith({List<HardwareDevice>? devices}) =>
      HardwareSnapshot(
        connected: connected,
        devices: devices ?? this.devices,
        updatedAt: updatedAt,
      );
}

/// Offline demo of the registry v4 list as the Mobile API shows it (only what is in the model, no doll axis).
abstract final class HardwareDemoData {
  static const snapshot = HardwareSnapshot(
    connected: false,
    updatedAt: 'ID 매핑 데모',
    devices: [
      HardwareDevice(
        id: 'SERVO-DOOR-01',
        name: '현관문',
        hardware: '열기/닫기 모터',
        controllerId: 'CTRL-ESP32-ACT-01',
        controllerName: 'ESP32',
        alias: 'door1',
        category: 'actuator',
        online: false,
        state: 'CLOSED',
        supportedCommands: ['OPEN', 'CLOSE'],
      ),
      HardwareDevice(
        id: 'SERVO-WINDOW-01',
        name: '창문',
        hardware: '열기/닫기 모터',
        controllerId: 'CTRL-ESP32-ACT-01',
        controllerName: 'ESP32',
        alias: 'wind1',
        category: 'actuator',
        online: false,
        state: 'CLOSED',
        supportedCommands: ['OPEN', 'CLOSE'],
      ),
      HardwareDevice(
        id: 'FAN-01',
        name: '환기팬',
        hardware: 'FAN',
        controllerId: 'CTRL-ESP8266-LED-01',
        controllerName: 'ESP8266',
        alias: 'pan1',
        category: 'actuator',
        online: false,
        state: 'OFF',
        supportedCommands: ['ON', 'OFF'],
      ),
      HardwareDevice(
        id: 'LED-AIRCON-01',
        name: '에어컨 표시',
        hardware: 'LED',
        controllerId: 'CTRL-ESP8266-LED-01',
        controllerName: 'ESP8266',
        alias: '에어컨',
        category: 'indicator',
        online: false,
        state: 'OFF',
        supportedCommands: ['ON', 'OFF'],
      ),
      HardwareDevice(
        id: 'LED-LIVING-01',
        name: '거실 조명',
        hardware: 'LED',
        controllerId: 'CTRL-ESP8266-LED-01',
        controllerName: 'ESP8266',
        alias: '조명',
        category: 'indicator',
        online: false,
        state: 'OFF',
        supportedCommands: ['ON', 'OFF'],
      ),
      HardwareDevice(
        id: 'MIC-INDOOR-01',
        name: '실내 음성 인식',
        hardware: 'USB 마이크',
        controllerName: '인식용 Raspberry Pi',
        alias: '음원 분석',
        category: 'input',
        online: false,
        state: 'UNKNOWN',
        supportedCommands: [],
      ),
      HardwareDevice(
        id: 'CAM-INDOOR-01',
        name: '내부 카메라',
        hardware: 'CAM',
        controllerName: '인식용 Raspberry Pi',
        alias: '내부',
        category: 'input',
        online: false,
        state: 'UNKNOWN',
        supportedCommands: [],
      ),
      HardwareDevice(
        id: 'CAM-OUTDOOR-01',
        name: '외부 카메라',
        hardware: 'CAM',
        controllerName: '인식용 Raspberry Pi',
        alias: '외부',
        category: 'input',
        online: false,
        state: 'UNKNOWN',
        supportedCommands: [],
      ),
    ],
  );
}
