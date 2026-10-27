import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/app/homemind_app.dart';
import 'package:homemind_application/core/api/homemind_api.dart';
import 'package:homemind_application/core/auth/pairing_credentials.dart';
import 'package:homemind_application/models/hardware_models.dart';
import 'package:homemind_application/state/app_controller.dart';

/// Server stand-in whose token can be revoked (Mobile API 0.4 answers 401).
class _RevocableApi extends HomeMindApi {
  bool revoked = false;

  @override
  Future<HardwareSnapshot> getDashboard() async {
    if (revoked) throw const HomeMindUnauthorizedException(tokenSent: true);
    return HardwareSnapshot(
      connected: true,
      devices: HardwareDemoData.snapshot.devices,
      updatedAt: 'now',
    );
  }
}

void main() {
  testWidgets('HomeMind starts behind the pairing gate', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(const HomeMindApp(autoConnect: false));
    await tester.pumpAndSettle();

    expect(find.text('HomeMind 연결'), findsOneWidget);
    expect(find.text('QR 코드 스캔'), findsOneWidget);
    expect(find.text('데모 모드로 둘러보기'), findsOneWidget);
    expect(find.text('하드웨어 제어'), findsNothing);
  });

  testWidgets('Demo mode renders only mapped hardware', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      const HomeMindApp(
        autoConnect: false,
        initialMode: HomeMindEntryMode.demo,
      ),
    );
    await tester.pumpAndSettle();

    expect(find.text('하드웨어 제어'), findsOneWidget);
    expect(find.text('현관문'), findsOneWidget);
    expect(find.text('SERVO-DOOR-01'), findsOneWidget);
    expect(find.text('장치'), findsOneWidget);
    expect(find.text('설정'), findsOneWidget);
    expect(find.text('난방을 켤까요?'), findsNothing);
    expect(find.text('Task'), findsNothing);
  });

  testWidgets('Settings shows hardware API boundary', (
    WidgetTester tester,
  ) async {
    await tester.pumpWidget(
      const HomeMindApp(
        autoConnect: false,
        initialMode: HomeMindEntryMode.demo,
      ),
    );
    await tester.tap(find.text('설정'));
    await tester.pumpAndSettle();

    expect(find.text('Pi 4 ID 매핑'), findsOneWidget);
    expect(find.text('GET /api/v1/hardware-map'), findsOneWidget);
    expect(find.text('MQTT'), findsOneWidget);
  });

  testWidgets('A paired phone opens the devices; a revoked token returns to pairing', (
    WidgetTester tester,
  ) async {
    final api = _RevocableApi();
    final store = MemoryCredentialStore(
      const PairingCredential(
        baseUrl: 'http://192.0.2.10:8000',
        token: 'hm1_test',
        tokenId: 'a1b2c3d4',
        deviceId: 'NODE-RPI4-01',
        deviceName: 'HomeMind 백엔드',
      ),
    );
    final controller = AppController(api: api, credentials: store);
    await tester.pumpWidget(HomeMindApp(controller: controller));
    await tester.pumpAndSettle();
    expect(find.text('하드웨어 제어'), findsOneWidget);
    expect(find.text('QR 코드 스캔'), findsNothing);

    api.revoked = true;
    await controller.refresh();
    await tester.pumpAndSettle();
    expect(find.text('HomeMind 연결'), findsOneWidget);
    expect(find.textContaining('다시 연결'), findsOneWidget);
    expect(store.value, isNull);
  });
}
