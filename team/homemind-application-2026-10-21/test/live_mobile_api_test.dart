// The app's real API client against a running Mobile API (C++ homemind-mobile-api or the Python reference) with
// core and the ESP controllers behind it. Skipped unless HOMEMIND_LIVE_QR_FILE names a file holding one fresh
// pairing QR payload; deploy/PHONE-APP/test-app-live.sh makes one, runs this test and revokes the token after.
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/core/api/homemind_api.dart';
import 'package:homemind_application/models/approval_models.dart';
import 'package:homemind_application/models/pairing_models.dart';

const _device = 'LED-LIVING-01';

Future<TaskStatus> _waitFinished(HomeMindApi api, DeviceCommandResult result) async {
  var status = TaskStatus(state: result.taskState ?? 'unknown');
  final deadline = DateTime.now().add(const Duration(seconds: 15));
  while (!status.finished && DateTime.now().isBefore(deadline)) {
    await Future<void>.delayed(const Duration(milliseconds: 300));
    status = await api.getTask(result.taskId!);
  }
  return status;
}

void main() {
  final qrFile = Platform.environment['HOMEMIND_LIVE_QR_FILE'];

  test(
    'QR pairing, dashboard and $_device ON/OFF through core against the live API',
    () async {
      final payload = PairingPayload.parse(File(qrFile!).readAsStringSync());
      final api = HomeMindApi(baseUrl: payload.baseUrl);
      addTearDown(api.close);

      if (payload.tlsSha256 != null) {
        // HTTPS: the self-signed certificate is trusted only through the QR's fingerprint
        final other = HomeMindApi(baseUrl: payload.baseUrl);
        addTearDown(other.close);
        await expectLater(other.healthCheck(), throwsA(isA<HomeMindApiException>()));
        other.setTlsPin('0' * 64);
        await expectLater(other.healthCheck(), throwsA(isA<HomeMindApiException>()));
        api.setTlsPin(payload.tlsSha256);
      }
      expect(await api.healthCheck(), isTrue);
      final claim = await api.claimPairing(payload, phoneName: 'live-test');
      expect(claim.deviceId, payload.deviceId);
      api.setToken(claim.token, forBaseUrl: payload.baseUrl);
      // one-time code: the same QR cannot pair a second phone
      await expectLater(
        api.claimPairing(payload, phoneName: 'live-test-again'),
        throwsA(isA<HomeMindPairingException>()),
      );

      final snapshot = await api.getDashboard();
      final led = snapshot.devices.singleWhere((device) => device.id == _device);
      expect(led.supportedCommands, containsAll(['ON', 'OFF']));

      for (final command in ['ON', 'OFF']) {
        final result = await api.sendDeviceCommand(_device, command);
        expect(result.taskId, isNotNull, reason: result.message);
        final status = await _waitFinished(api, result);
        expect(status.state, 'completed', reason: '$command: ${status.failureCode}');
      }
      final after = await api.getDashboard();
      expect(after.devices.singleWhere((device) => device.id == _device).state, 'OFF');

      // Mobile API 0.5: live events and approvals. Opening the entrance door from the phone waits for approval,
      // the phone may not approve it (403), but may reject it.
      final events = <LiveEvent>[];
      final subscription = api.liveEvents().listen(events.add);
      addTearDown(subscription.cancel);
      final door = await api.sendDeviceCommand('SERVO-DOOR-01', 'OPEN');
      final deadline = DateTime.now().add(const Duration(seconds: 10));
      ApprovalItem? waiting;
      while (waiting == null && DateTime.now().isBefore(deadline)) {
        await Future<void>.delayed(const Duration(milliseconds: 300));
        waiting = (await api.getApprovals())
            .where((item) => item.taskId == door.taskId)
            .firstOrNull;
      }
      expect(waiting, isNotNull, reason: 'door task not in GET /api/v1/approvals');
      expect(waiting!.phoneCanApprove, isFalse);
      expect(waiting.title, '현관문 열기');
      await expectLater(
        api.decideTask(door.taskId!, approve: true),
        throwsA(isA<HomeMindApiException>()),
      );
      await api.decideTask(door.taskId!, approve: false);
      expect((await _waitFinished(api, door)).state, 'rejected');
      await Future<void>.delayed(const Duration(milliseconds: 500));
      expect(events.first.type, 'open');
      expect(
        events.where((e) => e.type == 'task' && e.data['task_id'] == door.taskId).map((e) => e.data['state']),
        containsAll(['proposed', 'rejected']),
      );
      expect(events.where((e) => e.type == 'approvals'), isNotEmpty);

      api.setToken(null);
      await expectLater(
        api.getDashboard(),
        throwsA(isA<HomeMindUnauthorizedException>()),
      );
    },
    skip: qrFile == null
        ? 'set HOMEMIND_LIVE_QR_FILE (deploy/PHONE-APP/test-app-live.sh)'
        : false,
    timeout: const Timeout(Duration(minutes: 1)),
  );
}
