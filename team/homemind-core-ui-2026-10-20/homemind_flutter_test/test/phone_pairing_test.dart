import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/phone_pairing.dart';

import 'support/contract_files.dart';

void main() {
  test('pairing request matches mobile-pairing-request.schema.json', () {
    final payload = buildPairingRequestPayload(
      clientId: 'homemind-ui-7inch',
      eventId: '5f0e0000-0000-4000-8000-0000000000b1',
      requestId: '5f0e0000-0000-4000-8000-0000000000b0',
      sentAtMs: 1792560000000,
    );
    expect(schemaErrors(contractJson('mobile-pairing-request.schema.json'), payload), isEmpty);
    expect(payload, contractJson('examples/mobile-pairing-request.json'));
  });

  test('the contract example offer answers its own request only', () async {
    final offer = contractJson('examples/mobile-pairing-offer.json');
    final source = MqttHomeMindDataSource();
    final mine = source.debugWaitForPairing(offer['request_id'] as String);
    final other = source.debugWaitForPairing('5f0e0000-0000-4000-8000-0000000000ff');
    source.debugHandleMessage(kPairingOfferTopic, jsonEncode(offer));

    final got = await mine;
    expect(got.pairingCode, 'EXAM-PLE0');
    expect(got.qrPayload, offer['qr_payload']);
    expect(got.baseUrl, 'http://192.0.2.10:8000');
    expect(got.expiresAt.millisecondsSinceEpoch, offer['expires_at_ms']);
    expect(got.simulated, isFalse);
    // another request is still waiting (and times out with a Korean message)
    await expectLater(other, throwsA(isA<PhonePairingException>()));
    await source.dispose();
  });

  test('unavailable and malformed offers become Korean errors', () async {
    final source = MqttHomeMindDataSource();
    const busyId = '5f0e0000-0000-4000-8000-0000000000c1';
    final busy = source.debugWaitForPairing(busyId);
    source.debugHandleMessage(
        kPairingOfferTopic,
        jsonEncode({
          'schema_version': 'pfn.v1',
          'request_id': busyId,
          'target_node_id': 'NODE-RPI5-01',
          'status': 'unavailable',
          'reason': 'public_base_url is not set on the Mobile API',
        }));
    await expectLater(
        busy,
        throwsA(isA<PhonePairingException>().having(
            (error) => error.message, 'message', contains('public_base_url'))));

    const badId = '5f0e0000-0000-4000-8000-0000000000c2';
    final bad = source.debugWaitForPairing(badId);
    final broken = contractJson('examples/mobile-pairing-offer.json')
      ..['request_id'] = badId
      ..['pairing_code'] = 'not a code';
    source.debugHandleMessage(kPairingOfferTopic, jsonEncode(broken));
    await expectLater(bad, throwsA(isA<PhonePairingException>()));
    await source.dispose();
  });

  testWidgets('더보기 → 휴대폰 연결 shows the QR, the code and the simulation mark', (tester) async {
    tester.view.physicalSize = const Size(1024, 600);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(HomeMindApp(dataSource: MockHomeMindDataSource()));
    await tester.pump();
    await tester.tap(find.text('더보기').last);
    await tester.pumpAndSettle();
    await tester.tap(find.text('휴대폰 연결'));
    // the page ticks every second (countdown), so pump instead of pumpAndSettle
    for (var i = 0; i < 8; i++) {
      await tester.pump(const Duration(milliseconds: 100));
    }
    expect(find.byType(PairingQrCode), findsOneWidget);
    expect(find.text('DEMO-0001'), findsOneWidget);
    expect(find.text('시험용 QR (시뮬레이션)'), findsOneWidget);
    expect(find.textContaining('남음'), findsOneWidget);
  });

  test('phone list and unpair requests match mobile-phones-request.schema.json', () {
    final schema = contractJson('mobile-phones-request.schema.json');
    final unpair = buildPhonesRequestPayload(
      clientId: 'homemind-ui-7inch',
      eventId: '5f0e0000-0000-4000-8000-0000000000c1',
      requestId: '5f0e0000-0000-4000-8000-0000000000c0',
      sentAtMs: 1792560000000,
      revokePhoneId: '3f9a0c12',
    );
    expect(schemaErrors(schema, unpair), isEmpty);
    expect(unpair, contractJson('examples/mobile-phones-request.json'));
    final list = buildPhonesRequestPayload(
      clientId: 'homemind-ui-7inch',
      eventId: '5f0e0000-0000-4000-8000-0000000000d1',
      requestId: '5f0e0000-0000-4000-8000-0000000000d0',
      sentAtMs: 1792560000000,
    );
    expect(schemaErrors(schema, list), isEmpty);
    expect(list['action'], 'list');
    expect(list.containsKey('phone_id'), isFalse);
  });

  test('the contract example phone list answers its own request only', () async {
    final state = contractJson('examples/mobile-phones-state.json');
    final source = MqttHomeMindDataSource();
    final mine = source.debugWaitForPhones(state['request_id'] as String);
    final other = source.debugWaitForPhones('5f0e0000-0000-4000-8000-0000000000ff');
    source.debugHandleMessage(kPhonesStateTopic, jsonEncode(state));

    final got = await mine;
    expect(got.revoked, isTrue);
    expect(got.simulated, isFalse);
    expect(got.phones.single.phoneId, 'a1b2c3d4');
    expect(got.phones.single.deviceName, 'Galaxy S24');
    expect(got.phones.single.pairedAt, DateTime.utc(2026, 10, 25, 7, 30, 12).toLocal());
    expect(got.phones.single.lastSeenAt!.millisecondsSinceEpoch, 1792559990000);
    await expectLater(other, throwsA(isA<PhonePairingException>()));
    await source.dispose();
  });

  test('a list sent after a pairing reaches pairedPhoneUpdates; bad answers become Korean errors', () async {
    final source = MqttHomeMindDataSource();
    final update = source.pairedPhoneUpdates.first;
    final paired = contractJson('examples/mobile-phones-state.json')
      ..['request_id'] = null
      ..['action'] = 'paired'
      ..remove('revoked');
    expect(schemaErrors(contractJson('mobile-phones-state.schema.json'), paired), isEmpty);
    source.debugHandleMessage(kPhonesStateTopic, jsonEncode(paired));
    expect((await update).phones.single.deviceName, 'Galaxy S24');

    const busyId = '5f0e0000-0000-4000-8000-0000000000e1';
    final busy = source.debugWaitForPhones(busyId);
    source.debugHandleMessage(
        kPhonesStateTopic,
        jsonEncode({
          'request_id': busyId,
          'target_node_id': 'NODE-RPI5-01',
          'status': 'unavailable',
          'reason': 'the phone could not be unpaired on the Mobile API',
          'phones': [],
        }));
    await expectLater(
        busy,
        throwsA(isA<PhonePairingException>()
            .having((error) => error.message, 'message', contains('연결을 끊지 못했어요'))));

    const badId = '5f0e0000-0000-4000-8000-0000000000e2';
    final bad = source.debugWaitForPhones(badId);
    final leaky = contractJson('examples/mobile-phones-state.json')..['request_id'] = badId;
    (leaky['phones'] as List).first['phone_id'] = 'not-a-token-id';
    source.debugHandleMessage(kPhonesStateTopic, jsonEncode(leaky));
    await expectLater(bad, throwsA(isA<PhonePairingException>()));
    await source.dispose();
  });

  testWidgets('더보기 → 연결된 휴대폰 lists the phones and unpairs one after confirming', (tester) async {
    tester.view.physicalSize = const Size(1024, 600);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    await tester.pumpWidget(HomeMindApp(dataSource: MockHomeMindDataSource()));
    await tester.pump();
    await tester.tap(find.text('더보기').last);
    await tester.pumpAndSettle();
    await tester.tap(find.text('연결된 휴대폰'));
    await tester.pumpAndSettle();
    expect(find.text('시험용 목록 (시뮬레이션)'), findsOneWidget);
    expect(find.text('Galaxy S24 (데모)'), findsOneWidget);
    expect(find.text('iPhone 15 (데모)'), findsOneWidget);
    expect(find.textContaining('2대 연결됨'), findsOneWidget);
    expect(find.textContaining('최근 접속 3분 전'), findsOneWidget);
    expect(find.textContaining('최근 접속 기록 없음'), findsOneWidget);

    // by label: OutlinedButton.icon is a private subclass on the Pi's Flutter 3.29.3, so find.byType misses it
    await tester.tap(find.text('연결 해제').last);
    await tester.pumpAndSettle();
    expect(find.text('iPhone 15 (데모) 연결 해제'), findsOneWidget);
    await tester.tap(find.widgetWithText(TextButton, '취소'));
    await tester.pumpAndSettle();
    expect(find.text('iPhone 15 (데모)'), findsOneWidget);

    await tester.tap(find.text('연결 해제').last);
    await tester.pumpAndSettle();
    await tester.tap(find.widgetWithText(FilledButton, '연결 해제'));
    await tester.pumpAndSettle();
    expect(find.text('iPhone 15 (데모)'), findsNothing);
    expect(find.text('iPhone 15 (데모)의 연결을 끊었어요.'), findsOneWidget);
    expect(find.textContaining('1대 연결됨'), findsOneWidget);
  });
}
