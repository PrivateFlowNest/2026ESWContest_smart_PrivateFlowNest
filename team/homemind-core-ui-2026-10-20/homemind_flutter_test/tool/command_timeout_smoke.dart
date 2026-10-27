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

void _publish(MqttClient client, String topic, Map<String, dynamic> body) {
  final payload = MqttClientPayloadBuilder()..addString(jsonEncode(body));
  client.publishMessage(topic, MqttQos.atLeastOnce, payload.payload!);
}

Future<void> main(List<String> arguments) async {
  final host = arguments.isEmpty ? '127.0.0.1' : arguments[0];
  final port = arguments.length < 2 ? 28883 : int.parse(arguments[1]);
  final suffix = DateTime.now().millisecondsSinceEpoch;
  final clientId = 'timeout-smoke-$suffix';
  final client = MqttServerClient.withPort(host, clientId, port)
    ..connectTimeoutPeriod = 5000
    ..keepAlivePeriod = 5
    ..logging(on: false)
    ..connectionMessage =
        MqttConnectMessage().withClientIdentifier(clientId).startClean();
  final updates = StreamController<Map<String, dynamic>>.broadcast();
  final commands = StreamController<Map<String, dynamic>>.broadcast();
  StreamSubscription<List<MqttReceivedMessage<MqttMessage?>>>? subscription;
  try {
    await client.connect();
    if (client.connectionStatus?.state != MqttConnectionState.connected) {
      throw StateError('MQTT connection failed: ${client.connectionStatus}');
    }
    client.subscribe('pfn/v1/core/task/update', MqttQos.atLeastOnce);
    client.subscribe(
      'pfn/v1/device/CTRL-ESP8266-LED-01/command',
      MqttQos.atLeastOnce,
    );
    subscription = client.updates!.listen((receivedMessages) {
      for (final received in receivedMessages) {
        final message = received.payload;
        if (message is! MqttPublishMessage) continue;
        final payload = MqttPublishPayload.bytesToStringAsString(
          message.payload.message,
        );
        final decoded = jsonDecode(payload);
        if (decoded is! Map) continue;
        final body = Map<String, dynamic>.from(decoded);
        if (received.topic == 'pfn/v1/core/task/update') {
          updates.add(body);
        } else if (received.topic.endsWith('/command')) {
          commands.add(body);
        }
      }
    });
    await Future<void>.delayed(const Duration(milliseconds: 400));

    final taskId = _uuid();
    final traceId = _uuid();
    final proposedFuture = updates.stream.firstWhere((update) {
      final task = update['task'];
      return task is Map &&
          task['task_id'] == taskId &&
          task['state'] == 'proposed';
    }).timeout(const Duration(seconds: 5));
    _publish(client, 'pfn/v1/core/task/request', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': traceId,
      'task_id': taskId,
      'source_node_id': 'NODE-RPI5-01',
      'intent': 'night_bed_exit',
      'target_device': 'HOME-ENV-01',
      'priority': 70,
      'safety_level': 'normal',
      'interruptible': true,
      'resume_policy': 'automatic',
      'confidence': 1.0,
      'parameters': <String, dynamic>{'source': 'command_timeout_smoke'},
      'requested_at_ms': DateTime.now().millisecondsSinceEpoch,
    });
    final proposed = await proposedFuture;
    final proposedTask = Map<String, dynamic>.from(proposed['task'] as Map);

    final attempts = <int>[];
    final commandIds = <String>{};
    final commandSubscription = commands.stream.listen((command) {
      if (command['task_id'] != taskId) return;
      attempts.add((command['attempt'] as num).toInt());
      commandIds.add(command['command_id'].toString());
    });
    final failedFuture = updates.stream.firstWhere((update) {
      final task = update['task'];
      return task is Map &&
          task['task_id'] == taskId &&
          task['state'] == 'failed';
    }).timeout(const Duration(seconds: 16));
    _publish(client, 'pfn/v1/ui/action', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': traceId,
      'task_id': taskId,
      'client_id': clientId,
      'action': 'approve',
      'expected_revision': proposedTask['revision'],
      'action_token': proposedTask['action_token'],
      'reason': 'Verify three attempts and ACK timeout',
      'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
    });
    final failed = await failedFuture;
    await Future<void>.delayed(const Duration(milliseconds: 300));
    await commandSubscription.cancel();

    final task = Map<String, dynamic>.from(failed['task'] as Map);
    final failure = Map<String, dynamic>.from(task['failure'] as Map);
    attempts.sort();
    if (attempts.join(',') != '1,2,3') {
      throw StateError('Expected attempts 1,2,3 but got $attempts');
    }
    if (commandIds.length != 1) {
      throw StateError('Retries changed command_id: $commandIds');
    }
    if (failure['code'] != 'ACK_TIMEOUT' || failure['retry_allowed'] != true) {
      throw StateError('Unexpected failure payload: $failure');
    }
    stdout.writeln(jsonEncode({
      'result': 'COMMAND_TIMEOUT_SMOKE_PASS',
      'attempts': attempts,
      'one_command_id': commandIds.single,
      'failure': failure,
    }));
  } finally {
    await subscription?.cancel();
    await updates.close();
    await commands.close();
    client.disconnect();
  }
}
