import 'dart:convert';
import 'dart:io';

import 'package:crypto/crypto.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

/// Opt-in, against a broker with homemind-mobile-api on it (PC: demo/pc compose + deploy/PHONE-APP/pc-mobile-api.sh;
/// run through deploy/PHONE-APP/test-ui-qr-live.sh, which revokes the test token afterwards):
///   flutter test test/phone_pairing_live_test.dart \
///     --dart-define=RUN_BROKER_LIVE=true --dart-define=HOMEMIND_PI4_HOST=127.0.0.1
void main() {
  const runLive = bool.fromEnvironment('RUN_BROKER_LIVE');
  const host =
      String.fromEnvironment('HOMEMIND_PI4_HOST', defaultValue: '127.0.0.1');

  test(
    'the 7-inch QR pairs a phone: MQTT offer from the Mobile API, then claim',
    () async {
      final suffix = DateTime.now().millisecondsSinceEpoch % 1000000;
      final ui = MqttHomeMindDataSource(
          host: host, clientId: 'homemind-ui-qr-check-$suffix');
      var http = HttpClient();
      try {
        await ui.start();
        await ui.snapshots
            .firstWhere((snapshot) => snapshot.connected)
            .timeout(const Duration(seconds: 10), onTimeout: () => ui.current);
        expect(ui.current.connected, isTrue);
        await Future<void>.delayed(const Duration(milliseconds: 300));

        final offer = await ui.requestPhonePairing();
        expect(offer.simulated, isTrue);  // PC Mobile API config: simulated (not the device)
        expect(offer.expiresAt.isAfter(DateTime.now()), isTrue);
        final qr = Map<String, dynamic>.from(jsonDecode(offer.qrPayload) as Map);
        expect(qr['pairing_code'], offer.pairingCode);
        expect(qr['base_url'], offer.baseUrl);
        if (offer.baseUrl.startsWith('https://')) {
          // Mobile API 0.5 HTTPS: trust exactly the certificate named in the QR, as the phone app does
          final pin = qr['tls_sha256'] as String;
          expect(pin, matches(RegExp(r'^[0-9a-f]{64}$')));
          http.close(force: true);
          http = HttpClient(context: SecurityContext(withTrustedRoots: false))
            ..badCertificateCallback =
                (certificate, host, port) => sha256.convert(certificate.der).toString() == pin;
        }

        // what the phone app does after scanning the screen
        Future<(int, Map<String, dynamic>)> call(String method, String path,
            {Map<String, dynamic>? body, String? token}) async {
          final request =
              await http.openUrl(method, Uri.parse('${offer.baseUrl}$path'));
          request.headers.contentType = ContentType.json;
          if (token != null) request.headers.set('Authorization', 'Bearer $token');
          if (body != null) request.write(jsonEncode(body));
          final response = await request.close();
          final text = await utf8.decoder.bind(response).join();
          return (response.statusCode, Map<String, dynamic>.from(jsonDecode(text) as Map));
        }

        final claimBody = {
          'code': qr['pairing_code'],
          'nonce': qr['nonce'],
          'device_name': 'ui-qr-live-test',
        };
        final (status, claim) =
            await call('POST', '/api/v1/pairing/claim', body: claimBody);
        expect(status, 201);
        final token = claim['token'] as String;
        final (dashboard, _) = await call('GET', '/api/v1/dashboard', token: token);
        expect(dashboard, 200);
        // one-time: the same QR cannot pair a second phone
        final (again, _) = await call('POST', '/api/v1/pairing/claim',
            body: {...claimBody, 'device_name': 'ui-qr-live-test-2'});
        expect(again, 401);
      } finally {
        http.close(force: true);
        await ui.dispose();
      }
    },
    skip: runLive ? false : 'set --dart-define=RUN_BROKER_LIVE=true',
    timeout: const Timeout(Duration(seconds: 60)),
  );
}
