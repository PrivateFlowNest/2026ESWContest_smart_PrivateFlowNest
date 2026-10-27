import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

void main() {
  const runLive = bool.fromEnvironment('RUN_PI4_LIVE');

  test(
    'live UI data source disconnects and reconnects after MQTT restart',
    () async {
      final source = MqttHomeMindDataSource(
        clientId:
            'homemind-ui-reconnect-${DateTime.now().millisecondsSinceEpoch}',
      );
      final states = StreamController<bool>.broadcast();
      final subscription = source.snapshots.listen((snapshot) {
        states.add(snapshot.connected);
      });
      try {
        final initiallyConnected = states.stream
            .firstWhere((connected) => connected)
            .timeout(const Duration(seconds: 15));
        await source.start();
        await initiallyConnected;
        await states.stream
            .firstWhere((connected) => !connected)
            .timeout(const Duration(seconds: 25));
        await states.stream
            .firstWhere((connected) => connected)
            .timeout(const Duration(seconds: 40));
        expect(source.current.connected, isTrue);
        expect(source.current.connectionDetail, '실제 MQTT');
      } finally {
        await subscription.cancel();
        await states.close();
        await source.dispose();
      }
    },
    skip: !runLive,
    timeout: const Timeout(Duration(minutes: 2)),
  );
}
