import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

final Random _random = Random.secure();

String uuid() {
  final bytes = List<int>.generate(16, (_) => _random.nextInt(256));
  bytes[6] = (bytes[6] & 0x0f) | 0x40;
  bytes[8] = (bytes[8] & 0x3f) | 0x80;
  final hex =
      bytes.map((value) => value.toRadixString(16).padLeft(2, '0')).join();
  return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-'
      '${hex.substring(12, 16)}-${hex.substring(16, 20)}-'
      '${hex.substring(20)}';
}

void publish(
  MqttClient client,
  String topic,
  Map<String, dynamic> document,
) {
  final builder = MqttClientPayloadBuilder()..addString(jsonEncode(document));
  client.publishMessage(topic, MqttQos.atLeastOnce, builder.payload!);
}

Future<void> main() async {
  const host =
      String.fromEnvironment('HOMEMIND_PI4_HOST', defaultValue: '10.10.10.2');
  const port = int.fromEnvironment('HOMEMIND_MQTT_WS_PORT', defaultValue: 9001);
  const uiClientId = 'homemind-ui-smoke-01';
  final mqttClientId =
      'homemind-ws-smoke-${DateTime.now().millisecondsSinceEpoch % 100000}';
  final client = MqttServerClient.withPort('ws://$host', mqttClientId, port)
    ..useWebSocket = true
    ..websocketProtocols = MqttClientConstants.protocolsSingleDefault
    ..keepAlivePeriod = 5
    ..connectTimeoutPeriod = 4000
    ..logging(on: false)
    ..connectionMessage =
        MqttConnectMessage().withClientIdentifier(mqttClientId).startClean();

  await client.connect();
  if (client.connectionStatus?.state != MqttConnectionState.connected) {
    throw StateError(
        'WebSocket MQTT connection failed: ${client.connectionStatus}');
  }

  final snapshots = StreamController<Map<String, dynamic>>.broadcast();
  final taskUpdates = StreamController<Map<String, dynamic>>.broadcast();
  client.subscribe(
    'pfn/v1/core/state/response/$uiClientId',
    MqttQos.atLeastOnce,
  );
  client.subscribe('pfn/v1/core/task/update', MqttQos.atLeastOnce);
  final subscription = client.updates!.listen((messages) {
    for (final received in messages) {
      final message = received.payload as MqttPublishMessage;
      final payload = MqttPublishPayload.bytesToStringAsString(
        message.payload.message,
      );
      final decoded = jsonDecode(payload);
      if (decoded is! Map) continue;
      final data = Map<String, dynamic>.from(decoded);
      if (received.topic.startsWith('pfn/v1/core/state/response/')) {
        snapshots.add(data);
      } else if (received.topic == 'pfn/v1/core/task/update') {
        taskUpdates.add(data);
      }
    }
  });

  await Future<void>.delayed(const Duration(milliseconds: 300));
  final snapshotFuture =
      snapshots.stream.first.timeout(const Duration(seconds: 5));
  final traceId = uuid();
  publish(client, 'pfn/v1/core/state/request', {
    'schema_version': 'pfn.v1',
    'event_id': uuid(),
    'trace_id': traceId,
    'request_id': uuid(),
    'client_id': uiClientId,
    'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
  });
  final snapshot = await snapshotFuture;

  final taskId = uuid();
  final taskTraceId = uuid();
  final proposedFuture = taskUpdates.stream.firstWhere((update) {
    final task = update['task'];
    return task is Map &&
        task['task_id'] == taskId &&
        task['state'] == 'proposed';
  }).timeout(const Duration(seconds: 5));
  publish(client, 'pfn/v1/core/task/request', {
    'schema_version': 'pfn.v1',
    'event_id': uuid(),
    'trace_id': taskTraceId,
    'task_id': taskId,
    'source_node_id': 'NODE-RPI5-01',
    'intent': 'prepare_sleep',
    'target_device': 'HOME-ENV-01',
    'priority': 40,
    'safety_level': 'normal',
    'interruptible': true,
    'resume_policy': 'automatic',
    'confidence': 1.0,
    'parameters': <String, dynamic>{'source': 'websocket_smoke_test'},
    'requested_at_ms': DateTime.now().millisecondsSinceEpoch,
  });
  final proposed = await proposedFuture;
  final proposedTask = Map<String, dynamic>.from(proposed['task'] as Map);

  final rejectedFuture = taskUpdates.stream.firstWhere((update) {
    final task = update['task'];
    return task is Map &&
        task['task_id'] == taskId &&
        task['state'] == 'rejected';
  }).timeout(const Duration(seconds: 5));
  publish(client, 'pfn/v1/ui/action', {
    'schema_version': 'pfn.v1',
    'event_id': uuid(),
    'trace_id': taskTraceId,
    'task_id': taskId,
    'client_id': uiClientId,
    'action': 'reject',
    'expected_revision': proposedTask['revision'],
    'action_token': proposedTask['action_token'],
    'reason': 'WebSocket UI smoke test',
    'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
  });
  await rejectedFuture;

  final taskCount = (snapshot['tasks'] as List?)?.length ?? 0;
  final deviceCount = (snapshot['devices'] as List?)?.length ?? 0;
  stdout.writeln(
    'MQTT_WEBSOCKET_UI_SMOKE_PASS '
    'snapshot_tasks=$taskCount snapshot_devices=$deviceCount '
    'task_proposed_and_rejected=1',
  );

  await subscription.cancel();
  await snapshots.close();
  await taskUpdates.close();
  client.disconnect();
}
