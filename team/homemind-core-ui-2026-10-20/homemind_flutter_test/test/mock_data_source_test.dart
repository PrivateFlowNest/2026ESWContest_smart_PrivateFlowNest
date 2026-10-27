import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';

void main() {
  test('arrival scenario finishes and turns devices on', () async {
    final source = MockHomeMindDataSource();
    await source.start();
    await source.simulateArrival();

    expect(source.current.zone, HomeZone.living);
    expect(source.current.tasks.first.state, HomeTaskState.completed);
    expect(
      source.current.devices
          .firstWhere((device) => device.id == 'LED-LIVING-01')
          .active,
      isTrue,
    );

    await source.dispose();
  });

  test('sleep proposal requires user approval', () async {
    final source = MockHomeMindDataSource();
    await source.proposeSleep();

    expect(source.current.tasks.first.state, HomeTaskState.proposed);
    await source.actOnTask('sleep-demo-task', TaskAction.approve);
    expect(source.current.tasks.first.state, HomeTaskState.completed);

    await source.dispose();
  });

  test('command timeout exposes a useful failure reason', () async {
    final source = MockHomeMindDataSource();
    await source.simulateCommandTimeout();

    expect(source.current.tasks.first.state, HomeTaskState.failed);
    expect(source.current.tasks.first.failure?.code, 'ACK_TIMEOUT');
    expect(source.current.tasks.first.failure?.message, contains('3번'));

    await source.dispose();
  });

  test('15 second heartbeat loss marks ESP32 devices offline', () async {
    final source = MockHomeMindDataSource();
    await source.simulateDeviceOffline();

    final esp32Devices = source.current.devices
        .where((device) => device.controllerNodeId == 'CTRL-ESP32-ACT-01');
    expect(esp32Devices, isNotEmpty);
    expect(esp32Devices.every((device) => !device.online), isTrue);
    expect(source.current.systemStatus.overallStatus, 'degraded');

    await source.dispose();
  });

  test('mock lists every shown official registry device ID', () async {
    final source = MockHomeMindDataSource();
    final ids = source.current.devices.map((device) => device.id).toSet();

    // registry v4: 21 IDs, minus 9 not in the model and the 4 doll axis IDs (not listed)
    expect(ids.length, 8);
    expect(
        ids,
        containsAll(<String>[
          'SERVO-DOOR-01',
          'SERVO-WINDOW-01',
          'FAN-01',
          'LED-LIVING-01',
          'LED-AIRCON-01',
          'MIC-INDOOR-01',
          'CAM-INDOOR-01',
          'CAM-OUTDOOR-01',
        ]));
    expect(ids, isNot(contains('SERVO-DOOR-02')));

    await source.dispose();
  });
}
