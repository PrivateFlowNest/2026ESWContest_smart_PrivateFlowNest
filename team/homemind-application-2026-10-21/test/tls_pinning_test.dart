import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/core/api/homemind_api.dart';
import 'package:homemind_application/core/api/sha256.dart';
import 'package:homemind_application/core/auth/pairing_credentials.dart';
import 'package:homemind_application/models/pairing_models.dart';

String _qr({String baseUrl = 'https://192.168.0.14:8000', Object? tls}) =>
    jsonEncode({
      'version': 1,
      'device_id': 'NODE-RPI4-01',
      'device_name': 'HomeMind',
      'base_url': baseUrl,
      'pairing_code': 'ABCD-EFGH',
      'nonce': 'v1.2000000000.00112233445566778899aabbccddeeff',
      'expires_at': '2033-05-18T03:33:20Z',
      'tls_sha256': ?tls,
    });

void main() {
  test('SHA-256 matches FIPS 180-4 vectors', () {
    expect(
      sha256Hex(const []),
      'e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855',
    );
    expect(
      sha256Hex(utf8.encode('abc')),
      'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad',
    );
    expect(
      sha256Hex(
        utf8.encode('abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq'),
      ),
      '248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1',
    );
    expect(
      sha256Hex(List<int>.filled(1000000, 0x61)),
      'cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0',
    );
    // padding boundaries (same values as the C++ Mobile API test)
    expect(
      sha256Hex(List<int>.filled(55, 0x78)),
      'd5e285683cd4efc02d021a5c62014694958901005d6f71e89e0989fac77e4072',
    );
    expect(
      sha256Hex(List<int>.filled(64, 0x78)),
      '7ce100971f64e7001e8fe5a51973ecdfe1ced42befe7ee8d5fd6219506b5393c',
    );
  });

  test('only the pinned certificate is accepted', () {
    final der = utf8.encode('abc');
    const pin =
        'ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad';
    expect(certificateMatchesPin(der, pin), isTrue);
    expect(certificateMatchesPin(der, pin.toUpperCase()), isTrue);
    expect(certificateMatchesPin(utf8.encode('abd'), pin), isFalse);
    expect(certificateMatchesPin(der, null), isFalse);
  });

  test(
    'QR fingerprint: kept for https, dropped for http, refused when malformed',
    () {
      final pin = 'AB' * 32;
      expect(PairingPayload.parse(_qr(tls: pin)).tlsSha256, 'ab' * 32);
      expect(PairingPayload.parse(_qr()).tlsSha256, isNull);
      expect(
        PairingPayload.parse(_qr(baseUrl: 'http://192.168.0.14:8000', tls: pin))
            .tlsSha256,
        isNull,
      );
      expect(
        () => PairingPayload.parse(_qr(tls: 'abc')),
        throwsA(isA<PairingPayloadException>()),
      );
      expect(
        () => PairingPayload.parse(_qr(tls: 42)),
        throwsA(isA<PairingPayloadException>()),
      );
    },
  );

  test('stored pairing keeps the fingerprint', () {
    final credential = PairingCredential(
      baseUrl: 'https://192.168.0.14:8000',
      token: 't',
      tokenId: 'i',
      deviceId: 'NODE-RPI4-01',
      deviceName: 'HomeMind',
      tlsSha256: 'ab' * 32,
    );
    final back = PairingCredential.fromJson(credential.toJson());
    expect(back.tlsSha256, 'ab' * 32);
    final plain = PairingCredential.fromJson({
      'base_url': 'http://x',
      'token': 't',
    });
    expect(plain.tlsSha256, isNull);
    expect(plain.toJson().containsKey('tls_sha256'), isFalse);
  });
}
