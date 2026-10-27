import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

void main() {
  const traceId = '22222222-2222-4222-8222-222222222222';
  const taskId = '33333333-3333-4333-8333-333333333333';

  Map<String, dynamic> task(String state, int revision) => {
        'task_id': taskId,
        'trace_id': traceId,
        'intent': 'prepare_home',
        'target_device': 'HOME-ENV-01',
        'confidence': 0.94,
        'state': state,
        'priority': 40,
        'revision': revision,
        'action_token': '1234567890abcdef1234567890abcdef',
        'updated_at_ms': 1234,
      };

  test('maps Pi4 snapshot to tasks, zone and physical device status', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/core/state/response/homemind-ui-laptop-01',
      jsonEncode({
        'tasks': [task('proposed', 1)],
        'devices': [
          {
            'node_id': 'CTRL-ESP32-ACT-01',
            'status': 'online',
          },
          {
            'node_id': 'CTRL-ESP8266-LED-01',
            'status': 'online',
          },
        ],
        'world_state': [
          {
            'state_key': 'person_presence:entrance',
            'updated_at_ms': 100,
          },
        ],
        'safety': {
          'active_count': 0,
          'active_alerts': <dynamic>[],
        },
        'clock': {
          'valid': false,
          'boot_id': '11111111-1111-4111-8111-111111111111',
          'uptime_ms': 5000,
          'became_valid_at_ms': null,
        },
        'privacy': {
          'configured': true,
          'mode': 'camera_blocked',
          'camera_allowed': false,
          'microphone_allowed': true,
          'recording': false,
          'local_processing': true,
          'changed_at_ms': 2000,
        },
      }),
    );

    expect(source.current.tasks.single.state, HomeTaskState.proposed);
    expect(source.current.tasks.single.actionToken, isNotEmpty);
    expect(source.current.tasks.single.targetDevice, 'HOME-ENV-01');
    expect(source.current.tasks.single.confidence, 0.94);
    expect(source.current.tasks.single.updatedAt?.millisecondsSinceEpoch, 1234);
    expect(source.current.systemStatus.timeValid, isFalse);
    expect(source.current.systemStatus.bootUptimeMs, 5000);
    expect(source.current.zone, HomeZone.entrance);
    expect(source.current.privacy.mode, PrivacyMode.cameraBlocked);
    expect(source.current.privacy.cameraAllowed, isFalse);
    expect(source.current.privacy.microphoneAllowed, isTrue);
    expect(
      source.current.devices
          .where((device) =>
              device.controllerNodeId == 'CTRL-ESP32-ACT-01' ||
              device.controllerNodeId == 'CTRL-ESP8266-LED-01')
          .every((device) => device.online),
      isTrue,
    );
    expect(
      source.current.devices
          .where((device) => device.controllerNodeId == 'NODE-RPI5-02')
          .every((device) => !device.online),
      isTrue,
    );

    await source.dispose();
  });

  test('maps live privacy state and safety details', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/system/privacy/state',
      jsonEncode({
        'mode': 'private',
        'camera_allowed': false,
        'microphone_allowed': false,
        'recording': false,
        'local_processing': true,
        'changed_at_ms': 3000,
      }),
    );
    source.debugHandleMessage(
      'pfn/v1/system/safety/alert',
      jsonEncode({
        'alert_id': '44444444-4444-4444-8444-444444444444',
        'trace_id': traceId,
        'alert_type': 'manual_test',
        'zone': 'bathroom',
        'summary': '환기 팬 안전 점검',
        'raised_at_ms': 4000,
        'automatic_actions': [
          {'action': 'turn_on_warning_lights'}
        ],
      }),
    );

    expect(source.current.privacy.mode, PrivacyMode.private);
    expect(source.current.privacy.cameraAllowed, isFalse);
    expect(source.current.privacy.microphoneAllowed, isFalse);
    expect(source.current.safetyAlert, isTrue);
    expect(source.current.safetyDetail?.zone, 'bathroom');
    expect(source.current.safetyDetail?.summary, '환기 팬 안전 점검');

    await source.dispose();
  });

  test('maps task update and applies completed scenario device state',
      () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/core/task/update',
      jsonEncode({
        'trace_id': traceId,
        'task': task('completed', 4),
      }),
    );

    expect(source.current.tasks.single.state, HomeTaskState.completed);
    expect(
      source.current.devices
          .firstWhere((device) => device.id == 'LED-LIVING-01')
          .active,
      isTrue,
    );
    expect(
      source.current.devices
          .firstWhere((device) => device.id == 'FAN-01')
          .displayValue,
      'ON', // registry v3: FAN-01 is on/off only
    );

    await source.dispose();
  });

  test('maps telemetry using official reading source IDs', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/device/CTRL-ESP32-ACT-01/telemetry',
      jsonEncode({
        'sent_at_ms': 1000,
        'readings': {'fan_level_percent': 42},
        'reading_sources': {'fan_level_percent': 'FAN-01'},
      }),
    );

    expect(source.current.sensorReadings.single.value, '42');
    expect(source.current.sensorReadings.single.sourceDeviceId, 'FAN-01');
    expect(
      source.current.devices
          .firstWhere((device) => device.id == 'FAN-01')
          .displayValue,
      '42%',
    );

    await source.dispose();
  });

  test('maps system health storage and components', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/system/health',
      jsonEncode({
        'overall_status': 'degraded',
        'generated_at_ms': 2000,
        'storage': {
          'used_percent': 72.5,
          'warning_threshold_percent': 70,
          'warning': true,
          'database_bytes': 4096,
        },
        'components': [
          {
            'component': 'sqlite',
            'status': 'online',
          }
        ],
      }),
    );

    expect(source.current.systemStatus.storageUsedPercent, 72.5);
    expect(source.current.systemStatus.storageWarning, isTrue);
    expect(source.current.systemStatus.components.single.id, 'sqlite');

    await source.dispose();
  });

  test('maps optional command failure details', () async {
    final source = MqttHomeMindDataSource();
    final failed = task('failed', 4)
      ..['failure'] = {
        'code': 'ACK_TIMEOUT',
        'message': 'No ACK after three attempts',
        'occurred_at_ms': 3000,
        'retry_allowed': true,
      };
    source.debugHandleMessage(
      'pfn/v1/core/task/update',
      jsonEncode({'trace_id': traceId, 'task': failed}),
    );

    expect(source.current.tasks.single.failure?.code, 'ACK_TIMEOUT');
    expect(source.current.tasks.single.failure?.retryAllowed, isTrue);

    await source.dispose();
  });
}
