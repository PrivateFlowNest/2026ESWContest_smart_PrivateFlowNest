import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/core/api/homemind_api.dart';
import 'package:homemind_application/core/auth/pairing_credentials.dart';
import 'package:homemind_application/models/pairing_models.dart';
import 'package:homemind_application/state/app_controller.dart';

/// Loopback HTTP stand-in for the Mobile API 0.4 (pairing claim + bearer token).
class _FakeMobileApi {
  late final HttpServer _server;
  final List<_Seen> seen = [];
  String validToken = 'hm1_${'ab' * 32}';
  int claimStatus = 201;

  String get baseUrl => 'http://127.0.0.1:${_server.port}';

  Future<void> start() async {
    _server = await HttpServer.bind(InternetAddress.loopbackIPv4, 0);
    _server.listen(_handle);
  }

  Future<void> close() => _server.close(force: true);

  Future<void> _handle(HttpRequest request) async {
    final body = await utf8.decoder.bind(request).join();
    final auth = request.headers.value(HttpHeaders.authorizationHeader);
    seen.add(_Seen(request.method, request.uri.path, auth, body));
    final response = request.response..headers.contentType = ContentType.json;
    void reply(int status, Object json) {
      response.statusCode = status;
      response.write(jsonEncode(json));
    }

    if (request.uri.path == '/api/v1/pairing/claim') {
      switch (claimStatus) {
        case 201:
          reply(201, {
            'token': validToken,
            'token_id': 'a1b2c3d4',
            'token_type': 'Bearer',
            'device_id': 'NODE-RPI4-01',
            'device_name': 'HomeMind 백엔드',
          });
        case 429:
          response.headers.set('Retry-After', '42');
          reply(429, {'detail': 'too many failed pairing attempts; try again later'});
        default:
          reply(claimStatus, {
            'detail': 'invalid, expired or already used pairing code',
          });
      }
    } else if (auth != 'Bearer $validToken') {
      response.headers.set('WWW-Authenticate', 'Bearer');
      reply(401, {'detail': 'invalid or revoked token'});
    } else if (request.uri.path == '/api/v1/dashboard') {
      reply(200, {
        'snapshot': {
          'connected': true,
          'devices': [
            {
              'id': 'FAN-01',
              'name': '환기팬',
              'hardware': 'FAN',
              'controller_name': 'ESP32',
              'category': 'actuator',
              'online': true,
              'state': 'OFF',
              'supported_commands': ['ON', 'OFF'],
            },
          ],
          'updated_at': '2026-10-22T00:00:00+00:00',
        },
      });
    } else {
      reply(404, {'detail': 'Not Found'});
    }
    await response.close();
  }
}

class _Seen {
  const _Seen(this.method, this.path, this.authorization, this.body);

  final String method;
  final String path;
  final String? authorization;
  final String body;
}

PairingPayload _qr(String baseUrl) => PairingPayload.parse(
  jsonEncode({
    'version': 1,
    'device_id': 'NODE-RPI4-01',
    'device_name': 'HomeMind 백엔드',
    'base_url': baseUrl,
    'pairing_code': 'NLI7-ENCI',
    'nonce': 'v1.2000000000.00112233445566778899aabbccddeeff',
    'expires_at': '2033-05-18T03:33:20Z',
  }),
  now: DateTime.utc(2030),
);

void main() {
  late _FakeMobileApi server;

  setUp(() async {
    server = _FakeMobileApi();
    await server.start();
  });

  tearDown(() => server.close());

  test('approved QR claims a token, stores it and sends it as bearer', () async {
    final store = MemoryCredentialStore();
    final controller = AppController(
      api: HomeMindApi(baseUrl: 'http://10.0.2.2:8000'),
      credentials: store,
    );
    await controller.completePairing(_qr(server.baseUrl), phoneName: 'Pixel');

    final claim = server.seen.firstWhere((s) => s.path == '/api/v1/pairing/claim');
    expect(claim.method, 'POST');
    expect(claim.authorization, isNull);
    expect(jsonDecode(claim.body), {
      'code': 'NLI7-ENCI',
      'nonce': 'v1.2000000000.00112233445566778899aabbccddeeff',
      'device_name': 'Pixel',
    });
    expect(store.value?.token, server.validToken);
    expect(store.value?.baseUrl, server.baseUrl);
    expect(store.value?.tokenId, 'a1b2c3d4');
    expect(controller.paired, isTrue);
    expect(controller.baseUrl, server.baseUrl);
    expect(controller.connected, isTrue);
    final dashboard = server.seen.lastWhere((s) => s.path == '/api/v1/dashboard');
    expect(dashboard.authorization, 'Bearer ${server.validToken}');
    expect(controller.snapshot.devices.single.id, 'FAN-01');
    controller.dispose();
  });

  test('stored token is restored on start', () async {
    final store = MemoryCredentialStore(
      PairingCredential(
        baseUrl: server.baseUrl,
        token: server.validToken,
        tokenId: 'a1b2c3d4',
        deviceId: 'NODE-RPI4-01',
        deviceName: 'HomeMind 백엔드',
      ),
    );
    final controller = AppController(api: HomeMindApi(), credentials: store);
    expect(await controller.restoreSession(), isTrue);
    expect(await controller.refresh(), isTrue);
    expect(server.seen.single.authorization, 'Bearer ${server.validToken}');
    controller.dispose();
  });

  test('401 (revoked token) clears the token and asks for pairing', () async {
    final store = MemoryCredentialStore(
      PairingCredential(
        baseUrl: server.baseUrl,
        token: 'hm1_revoked',
        tokenId: 'x',
        deviceId: 'NODE-RPI4-01',
        deviceName: 'HomeMind',
      ),
    );
    final controller = AppController(api: HomeMindApi(), credentials: store);
    await controller.restoreSession();
    var notified = false;
    controller.addListener(() => notified |= controller.needsPairing);
    expect(await controller.refresh(), isFalse);
    expect(controller.needsPairing, isTrue);
    expect(notified, isTrue);
    expect(controller.paired, isFalse);
    expect(store.value, isNull);
    expect(controller.lastError, contains('다시 연결'));
    expect(controller.usingDemoData, isTrue);
    controller.dispose();
  });

  test('token is never sent to another address', () async {
    final store = MemoryCredentialStore(
      PairingCredential(
        baseUrl: 'http://192.0.2.10:8000',
        token: server.validToken,
        tokenId: 'x',
        deviceId: 'NODE-RPI4-01',
        deviceName: 'HomeMind',
      ),
    );
    final controller = AppController(api: HomeMindApi(), credentials: store);
    await controller.restoreSession();
    expect(await controller.updateBaseUrl(server.baseUrl), isFalse);
    expect(server.seen.single.authorization, isNull);
    // the 401 came without our token: the stored pairing for the other address stays
    expect(controller.needsPairing, isTrue);
    expect(store.value?.token, server.validToken);
    controller.dispose();
  });

  test('refused claims explain what to do and store nothing', () async {
    final store = MemoryCredentialStore();
    final controller = AppController(api: HomeMindApi(), credentials: store);
    server.claimStatus = 401;
    await expectLater(
      controller.completePairing(_qr(server.baseUrl)),
      throwsA(
        isA<HomeMindPairingException>().having(
          (e) => e.message,
          'message',
          contains('새 QR'),
        ),
      ),
    );
    server.claimStatus = 429;
    await expectLater(
      controller.completePairing(_qr(server.baseUrl)),
      throwsA(
        isA<HomeMindPairingException>().having(
          (e) => e.message,
          'message',
          contains('42초'),
        ),
      ),
    );
    expect(store.value, isNull);
    expect(controller.paired, isFalse);
    expect(server.seen.where((s) => s.path != '/api/v1/pairing/claim'), isEmpty);
    controller.dispose();
  });

  test('forgetting the pairing removes the token locally', () async {
    final store = MemoryCredentialStore();
    final controller = AppController(api: HomeMindApi(), credentials: store);
    await controller.completePairing(_qr(server.baseUrl));
    await controller.forgetPairing();
    expect(store.value, isNull);
    expect(controller.needsPairing, isTrue);
    server.seen.clear();
    await controller.refresh();
    expect(server.seen.single.authorization, isNull);
    controller.dispose();
  });
}
