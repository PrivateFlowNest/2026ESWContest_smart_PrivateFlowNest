import 'dart:async';
import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/voice_listen.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

import 'support/contract_files.dart';

/// Opt-in, against a running broker (PC demo: docker compose demo/pc):
///   flutter test test/voice_listen_live_test.dart \
///     --dart-define=RUN_BROKER_LIVE=true --dart-define=HOMEMIND_PI4_HOST=127.0.0.1
void main() {
  const runLive = bool.fromEnvironment('RUN_BROKER_LIVE');
  const host =
      String.fromEnvironment('HOMEMIND_PI4_HOST', defaultValue: '127.0.0.1');

  test(
    'push-to-talk reaches a pfn/v1/ai/voice/listen subscriber, schema-valid',
    () async {
      final suffix = DateTime.now().millisecondsSinceEpoch % 1000000;
      final listener = MqttServerClient.withPort(
          host, 'homemind-voice-listen-check-$suffix', 1883)
        ..logging(on: false)
        ..keepAlivePeriod = 5
        ..setProtocolV311();
      await listener.connect();
      final received = Completer<Map<String, dynamic>>();
      listener.updates!.listen((messages) {
        for (final message in messages) {
          final publish = message.payload as MqttPublishMessage;
          if (message.topic != kVoiceListenTopic || received.isCompleted) {
            continue;
          }
          expect(publish.header?.retain, isFalse);
          expect(publish.header?.qos, MqttQos.atLeastOnce);
          received.complete(Map<String, dynamic>.from(jsonDecode(
                  MqttPublishPayload.bytesToStringAsString(
                      publish.payload.message))
              as Map));
        }
      });
      listener.subscribe(kVoiceListenTopic, MqttQos.atLeastOnce);

      final ui = MqttHomeMindDataSource(
          host: host, clientId: 'homemind-ui-7inch-check-$suffix');
      try {
        await ui.start();
        await ui.snapshots
            .firstWhere((snapshot) => snapshot.connected)
            .timeout(const Duration(seconds: 10), onTimeout: () => ui.current);
        expect(ui.current.connected, isTrue);
        await Future<void>.delayed(const Duration(milliseconds: 300));
        expect(await ui.requestVoiceListen(VoiceListenAction.start), isTrue);

        final payload =
            await received.future.timeout(const Duration(seconds: 5));
        expect(schemaErrors(contractJson('voice-listen.schema.json'), payload),
            isEmpty);
        expect(payload['action'], 'start');
        expect(payload['client_id'], 'homemind-ui-7inch-check-$suffix');
        final now = DateTime.now().millisecondsSinceEpoch;
        expect(payload['expires_at_ms'] as int, greaterThan(now));
      } finally {
        await ui.dispose();
        listener.disconnect();
      }
    },
    skip: !runLive,
    timeout: const Timeout(Duration(seconds: 40)),
  );
}
