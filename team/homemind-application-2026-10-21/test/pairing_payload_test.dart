import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/models/pairing_models.dart';

void main() {
  const validQr = '''
{
  "version": 1,
  "device_id": "homemind-pi4-001",
  "device_name": "거실 HomeMind",
  "base_url": "https://192.168.1.10:8000/",
  "pairing_code": "A7X9-K2MP",
  "nonce": "test-nonce",
  "expires_at": "2030-09-01T15:30:00Z"
}
''';

  test('valid HomeMind QR is parsed and normalized', () {
    final payload = PairingPayload.parse(
      validQr,
      now: DateTime.utc(2030, 9, 1, 15),
    );

    expect(payload.deviceId, 'homemind-pi4-001');
    expect(payload.deviceName, '거실 HomeMind');
    expect(payload.baseUrl, 'https://192.168.1.10:8000');
    expect(payload.pairingCode, 'A7X9-K2MP');
  });

  test('expired QR is rejected', () {
    expect(
      () => PairingPayload.parse(validQr, now: DateTime.utc(2030, 9, 1, 16)),
      throwsA(
        isA<PairingPayloadException>().having(
          (error) => error.message,
          'message',
          contains('만료'),
        ),
      ),
    );
  });

  test('non-http Pi address is rejected', () {
    final invalid = validQr.replaceFirst(
      'https://192.168.1.10:8000/',
      'mqtt://192.168.1.10',
    );

    expect(
      () => PairingPayload.parse(invalid, now: DateTime.utc(2030, 9, 1, 15)),
      throwsA(isA<PairingPayloadException>()),
    );
  });
}
