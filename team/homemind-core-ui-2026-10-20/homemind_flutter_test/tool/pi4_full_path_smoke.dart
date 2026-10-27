import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

final Random _random = Random.secure();

String _uuid() {
  final bytes = List<int>.generate(16, (_) => _random.nextInt(256));
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  final hex =
      bytes.map((value) => value.toRadixString(16).padLeft(2, '0')).join();
  return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-'
      '${hex.substring(12, 16)}-${hex.substring(16, 20)}-'
      '${hex.substring(20)}';
}

void _publish(
  MqttClient client,
  String topic,
  Map<String, dynamic> document, {
  MqttQos qos = MqttQos.atLeastOnce,
}) {
  final builder = MqttClientPayloadBuilder()..addString(jsonEncode(document));
  client.publishMessage(topic, qos, builder.payload!, retain: false);
}

Future<Map<String, dynamic>> _waitFor(
  Stream<MapEntry<String, Map<String, dynamic>>> messages,
  bool Function(String topic, Map<String, dynamic> body) predicate, {
  Duration timeout = const Duration(seconds: 15),
}) =>
    messages
        .firstWhere((message) => predicate(message.key, message.value))
        .then((message) => message.value)
        .timeout(timeout);

Future<void> main() async {
  const host = String.fromEnvironment(
    'HOMEMIND_PI4_HOST',
    defaultValue: '10.10.10.2',
  );
  const port = int.fromEnvironment(
    'HOMEMIND_MQTT_PORT',
    defaultValue: 1883,
  );
  final suffix = DateTime.now().millisecondsSinceEpoch;
  final uiClientId = 'homemind-ui-full-path-$suffix';
  final mqttClientId = 'homemind-full-path-$suffix';
  final client = MqttServerClient.withPort(host, mqttClientId, port)
    ..keepAlivePeriod = 5
    ..connectTimeoutPeriod = 5000
    ..logging(on: false)
    ..connectionMessage =
        MqttConnectMessage().withClientIdentifier(mqttClientId).startClean();

  final messages =
      StreamController<MapEntry<String, Map<String, dynamic>>>.broadcast();
  StreamSubscription<List<MqttReceivedMessage<MqttMessage?>>>? subscription;
  try {
    await client.connect();
    if (client.connectionStatus?.state != MqttConnectionState.connected) {
      throw StateError('MQTT connection failed: ${client.connectionStatus}');
    }

    for (final topic in [
      'pfn/v1/core/state/response/$uiClientId',
      'pfn/v1/core/task/update',
      'pfn/v1/device/+/ack',
      'pfn/v1/device/+/telemetry',
      'pfn/v1/system/heartbeat',
      'pfn/v1/system/health',
      'pfn/v1/system/privacy/state',
      'pfn/v1/system/safety/alert',
    ]) {
      client.subscribe(topic, MqttQos.atLeastOnce);
    }
    subscription = client.updates!.listen((receivedMessages) {
      for (final received in receivedMessages) {
        final message = received.payload;
        if (message is! MqttPublishMessage) continue;
        final payload = MqttPublishPayload.bytesToStringAsString(
          message.payload.message,
        );
        try {
          final decoded = jsonDecode(payload);
          if (decoded is Map) {
            messages.add(MapEntry(
              received.topic,
              Map<String, dynamic>.from(decoded),
            ));
          }
        } on FormatException {
          // Invalid external data is ignored and cannot stop the test client.
        }
      }
    });
    await Future<void>.delayed(const Duration(milliseconds: 500));

    final telemetryNodes = <String>{};
    final heartbeatNodes = <String>{};
    final ackNodes = <String>{};
    final observation = messages.stream.listen((message) {
      final node = message.value['node_id']?.toString();
      if (message.key.endsWith('/telemetry') && node != null) {
        telemetryNodes.add(node);
      } else if (message.key == 'pfn/v1/system/heartbeat' && node != null) {
        heartbeatNodes.add(node);
      } else if (message.key.endsWith('/ack') && node != null) {
        ackNodes.add(node);
      }
    });

    final snapshotFuture = _waitFor(
      messages.stream,
      (topic, _) => topic == 'pfn/v1/core/state/response/$uiClientId',
    );
    final traceId = _uuid();
    _publish(client, 'pfn/v1/core/state/request', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': traceId,
      'request_id': _uuid(),
      'client_id': uiClientId,
      'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
    });
    final snapshot = await snapshotFuture;
    final clockValue = snapshot['clock'];
    if (clockValue is! Map ||
        clockValue['valid'] is! bool ||
        clockValue['boot_id'] is! String ||
        clockValue['uptime_ms'] is! num) {
      throw StateError(
          'Snapshot clock status is missing or malformed: $clockValue');
    }
    final clock = Map<String, dynamic>.from(clockValue);

    final completedScenarios = <String>[];
    String? feedbackTaskId;
    String? feedbackTraceId;
    for (final intent in [
      'prepare_home',
      'prepare_sleep',
      'night_bed_exit',
    ]) {
      final taskId = _uuid();
      final taskTraceId = _uuid();
      final proposedFuture = _waitFor(messages.stream, (topic, body) {
        final task = body['task'];
        return topic == 'pfn/v1/core/task/update' &&
            task is Map &&
            task['task_id'] == taskId &&
            task['state'] == 'proposed';
      });
      _publish(client, 'pfn/v1/core/task/request', {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': taskTraceId,
        'task_id': taskId,
        'source_node_id': 'NODE-RPI5-01',
        'intent': intent,
        'target_device': 'HOME-ENV-01',
        'priority': intent == 'night_bed_exit' ? 70 : 40,
        'safety_level': 'normal',
        'interruptible': true,
        'resume_policy': 'automatic',
        'confidence': 1.0,
        'parameters': <String, dynamic>{'source': 'pi4_full_path_smoke'},
        'requested_at_ms': DateTime.now().millisecondsSinceEpoch,
      });
      final proposed = await proposedFuture;
      final proposedTask = Map<String, dynamic>.from(proposed['task'] as Map);
      final completedFuture = _waitFor(messages.stream, (topic, body) {
        final task = body['task'];
        return topic == 'pfn/v1/core/task/update' &&
            task is Map &&
            task['task_id'] == taskId &&
            task['state'] == 'completed';
      });
      _publish(client, 'pfn/v1/ui/action', {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': taskTraceId,
        'task_id': taskId,
        'client_id': uiClientId,
        'action': 'approve',
        'expected_revision': proposedTask['revision'],
        'action_token': proposedTask['action_token'],
        'reason': 'Automated full-path integration test',
        'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
      });
      await completedFuture;
      completedScenarios.add(intent);
      feedbackTaskId = taskId;
      feedbackTraceId = taskTraceId;
    }

    final privacyFuture = _waitFor(
      messages.stream,
      (topic, body) =>
          topic == 'pfn/v1/system/privacy/state' && body['mode'] == 'normal',
    );
    final privacyTraceId = _uuid();
    _publish(client, 'pfn/v1/ui/privacy/set', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': privacyTraceId,
      'client_id': uiClientId,
      'mode': 'normal',
      'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
    });
    final privacyState = await privacyFuture;

    final feedbackEventId = _uuid();
    _publish(client, 'pfn/v1/ui/feedback', {
      'schema_version': 'pfn.v1',
      'event_id': feedbackEventId,
      'trace_id': feedbackTraceId ?? _uuid(),
      'client_id': uiClientId,
      'feedback_type': 'action_feedback',
      'task_id': feedbackTaskId,
      'observation_event_id': null,
      'rating': 'positive',
      'note': 'Flutter live integration smoke',
      'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
    });

    final alertId = _uuid();
    final safetyTraceId = _uuid();
    final safetyFuture = _waitFor(
      messages.stream,
      (topic, body) =>
          topic == 'pfn/v1/system/safety/alert' && body['alert_id'] == alertId,
    );
    _publish(client, 'pfn/v1/system/safety/alert', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': safetyTraceId,
      'alert_id': alertId,
      'source_node_id': 'NODE-RPI5-01',
      'alert_type': 'manual_test',
      'zone': 'living',
      'severity': 'p0',
      'summary': 'Flutter live P0 integration test',
      'evidence': <String, dynamic>{'automated_test': true},
      'raised_at_ms': DateTime.now().millisecondsSinceEpoch,
      'automatic_actions': <dynamic>[],
    });
    await safetyFuture;
    _publish(client, 'pfn/v1/ui/safety/ack', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': safetyTraceId,
      'alert_id': alertId,
      'client_id': uiClientId,
      'action': 'dismiss_test',
      'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
    });

    await Future<void>.delayed(const Duration(milliseconds: 500));
    final finalSnapshotFuture = _waitFor(
      messages.stream,
      (topic, _) => topic == 'pfn/v1/core/state/response/$uiClientId',
    );
    _publish(client, 'pfn/v1/core/state/request', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': _uuid(),
      'request_id': _uuid(),
      'client_id': uiClientId,
      'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
    });
    final finalSnapshot = await finalSnapshotFuture;
    final finalSafety = finalSnapshot['safety'];
    if (finalSafety is! Map || finalSafety['active_count'] != 0) {
      throw StateError('P0 ACK did not clear the alert: $finalSafety');
    }

    await Future<void>.delayed(const Duration(seconds: 6));
    await observation.cancel();

    const expectedControllerNodes = {
      'CTRL-ESP32-ACT-01',
      'CTRL-ESP8266-LED-01',
    };
    if (!telemetryNodes.containsAll(expectedControllerNodes)) {
      throw StateError('Missing telemetry nodes: $telemetryNodes');
    }
    if (!heartbeatNodes.containsAll(expectedControllerNodes)) {
      throw StateError('Missing heartbeat nodes: $heartbeatNodes');
    }
    if (!ackNodes.containsAll(expectedControllerNodes)) {
      throw StateError('Missing ACK nodes: $ackNodes');
    }

    stdout.writeln(jsonEncode({
      'result': 'PI4_FULL_PATH_SMOKE_PASS',
      'snapshot_tasks': (snapshot['tasks'] as List?)?.length ?? 0,
      'snapshot_devices': (snapshot['devices'] as List?)?.length ?? 0,
      'clock_valid': clock['valid'],
      'clock_uptime_ms': clock['uptime_ms'],
      'completed_scenarios': completedScenarios,
      'privacy_mode': privacyState['mode'],
      'feedback_event_id': feedbackEventId,
      'p0_ack_cleared': true,
      'telemetry_nodes': telemetryNodes.toList()..sort(),
      'heartbeat_nodes': heartbeatNodes.toList()..sort(),
      'ack_nodes': ackNodes.toList()..sort(),
    }));
  } finally {
    await subscription?.cancel();
    await messages.close();
    client.disconnect();
  }
}
