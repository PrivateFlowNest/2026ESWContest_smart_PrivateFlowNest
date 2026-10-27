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

Map<String, dynamic> _health(String status, int generatedAtMs) => {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': _uuid(),
      'source_node_id': 'NODE-RPI4-TEST-ORDER',
      'overall_status': status,
      'components': [
        {'component': 'order-probe', 'status': status},
      ],
      'storage': {
        'used_percent': 10,
        'warning_threshold_percent': 70,
        'warning': false,
        'database_bytes': 1234,
      },
      'generated_at_ms': generatedAtMs,
    };

Future<void> main(List<String> arguments) async {
  final host = arguments.isEmpty ? '127.0.0.1' : arguments[0];
  final port = arguments.length < 2 ? 28884 : int.parse(arguments[1]);
  final clientId = 'latest-order-${DateTime.now().microsecondsSinceEpoch}';
  final responseTopic = 'pfn/v1/core/state/response/$clientId';
  final client = MqttServerClient.withPort(host, clientId, port)
    ..connectTimeoutPeriod = 5000
    ..keepAlivePeriod = 5
    ..logging(on: false)
    ..connectionMessage =
        MqttConnectMessage().withClientIdentifier(clientId).startClean();
  StreamSubscription<List<MqttReceivedMessage<MqttMessage?>>>? subscription;
  final responses = StreamController<Map<String, dynamic>>.broadcast();
  try {
    await client.connect();
    if (client.connectionStatus?.state != MqttConnectionState.connected) {
      throw StateError('MQTT connection failed: ${client.connectionStatus}');
    }
    client.subscribe(responseTopic, MqttQos.atLeastOnce);
    subscription = client.updates!.listen((messages) {
      for (final received in messages) {
        if (received.topic != responseTopic ||
            received.payload is! MqttPublishMessage) {
          continue;
        }
        final message = received.payload as MqttPublishMessage;
        final text = MqttPublishPayload.bytesToStringAsString(
          message.payload.message,
        );
        final decoded = jsonDecode(text);
        if (decoded is Map) {
          responses.add(Map<String, dynamic>.from(decoded));
        }
      }
    });
    await Future<void>.delayed(const Duration(milliseconds: 300));

    _publish(
        client, 'pfn/v1/system/health', _health('degraded', 4102444800000));
    await Future<void>.delayed(const Duration(milliseconds: 300));
    _publish(client, 'pfn/v1/system/health', _health('healthy', 1));
    await Future<void>.delayed(const Duration(milliseconds: 300));

    final requestId = _uuid();
    final responseFuture = responses.stream
        .firstWhere(
          (response) => response['request_id'] == requestId,
        )
        .timeout(const Duration(seconds: 5));
    _publish(client, 'pfn/v1/core/state/request', {
      'schema_version': 'pfn.v1',
      'event_id': _uuid(),
      'trace_id': _uuid(),
      'request_id': requestId,
      'client_id': clientId,
      'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
    });
    final response = await responseFuture;
    final health = Map<String, dynamic>.from(response['system_health'] as Map);
    if (health['overall_status'] != 'healthy' ||
        health['generated_at_ms'] != 1) {
      throw StateError('Wall clock incorrectly won latest ordering: $health');
    }
    stdout.writeln(jsonEncode({
      'result': 'LATEST_INPUT_ORDER_PASS',
      'overall_status': health['overall_status'],
      'generated_at_ms': health['generated_at_ms'],
    }));
  } finally {
    await subscription?.cancel();
    await responses.close();
    client.disconnect();
  }
}
