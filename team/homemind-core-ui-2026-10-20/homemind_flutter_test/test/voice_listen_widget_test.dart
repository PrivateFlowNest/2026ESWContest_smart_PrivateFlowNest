import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';

void _setDisplay(WidgetTester tester) {
  tester.view.physicalSize = const Size(1024, 600);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.resetPhysicalSize);
  addTearDown(tester.view.resetDevicePixelRatio);
}

/// Lets the mock finish its ready → running → completed delays.
Future<void> _settle(WidgetTester tester) async {
  for (var i = 0; i < 4; i++) {
    await tester.pump(const Duration(milliseconds: 400));
  }
  await tester.pumpAndSettle();
}

ButtonStyleButton _talkButton(WidgetTester tester, String label) =>
    tester.widget<ButtonStyleButton>(find.ancestor(
        of: find.text(label), matching: find.bySubtype<ButtonStyleButton>()));

class _UnreachableMock extends MockHomeMindDataSource {
  @override
  Future<bool> requestVoiceListen(VoiceListenAction action) async => false;
}

void main() {
  testWidgets('말하기 shows 듣는 중… until the voice task arrives',
      (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    expect(find.text('말하기'), findsOneWidget);
    await tester.tap(find.text('말하기'));
    await tester.pump();
    expect(find.text('듣는 중…'), findsOneWidget);
    expect(source.voiceListenRequests, [VoiceListenAction.start]);

    await tester.pump(const Duration(seconds: 1));
    expect(find.text('듣는 중…'), findsOneWidget);

    // The mock "hears" 거실 불 켜줘 after 1.5 s: a voice_parser task appears.
    await tester.pump(const Duration(milliseconds: 600));
    expect(find.text('듣는 중…'), findsNothing);
    expect(find.text('말하기'), findsOneWidget);
    await _settle(tester);
    expect(source.current.tasks.first.originKind, TaskOrigin.voice);
    expect(source.current.tasks.first.state, HomeTaskState.completed);
  });

  testWidgets('without an answer 듣는 중… ends after about 6 seconds',
      (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource()..simulateVoiceReply = false;
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    await tester.tap(find.text('말하기'));
    await tester.pump(const Duration(seconds: 5));
    expect(find.text('듣는 중…'), findsOneWidget);
    await tester.pump(const Duration(milliseconds: 1100));
    expect(find.text('듣는 중…'), findsNothing);
    expect(find.text('말하기'), findsOneWidget);
    expect(source.voiceListenRequests, [VoiceListenAction.start]);
  });

  testWidgets('pressing 듣는 중… again sends cancel', (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    await tester.tap(find.text('말하기'));
    await tester.pump();
    await tester.tap(find.text('듣는 중…'));
    await tester.pump();
    expect(find.text('말하기'), findsOneWidget);
    expect(source.voiceListenRequests,
        [VoiceListenAction.start, VoiceListenAction.cancel]);

    // The cancelled press never turns into a task.
    await tester.pump(const Duration(seconds: 3));
    expect(source.current.tasks, isEmpty);
  });

  testWidgets('microphone privacy mode and offline disable 말하기',
      (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource();
    await source.setPrivacyMode(PrivacyMode.audioBlocked);
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    expect(_talkButton(tester, '말하기').enabled, isFalse);
    expect(find.byIcon(Icons.mic_off), findsOneWidget);
    await tester.tap(find.text('말하기'), warnIfMissed: false);
    await tester.pump();
    expect(source.voiceListenRequests, isEmpty);

    await source.setPrivacyMode(PrivacyMode.normal);
    await tester.pump();
    expect(_talkButton(tester, '말하기').enabled, isTrue);

    await source.setConnected(false);
    await tester.pump();
    expect(_talkButton(tester, '말하기').enabled, isFalse);
  });

  testWidgets('a press that cannot be sent shows a message', (tester) async {
    _setDisplay(tester);
    await tester.pumpWidget(HomeMindApp(dataSource: _UnreachableMock()));
    await tester.pump();

    await tester.tap(find.text('말하기'));
    await tester.pump();
    expect(find.text('듣는 중…'), findsNothing);
    expect(find.text('Pi4 연결이 끊겨 음성 듣기를 시작하지 못했어요'), findsOneWidget);
  });

  testWidgets('header with 말하기 fits 1024x600 in the widest states',
      (tester) async {
    _setDisplay(tester);
    final offline = MockHomeMindDataSource(seedDemoData: true);
    await offline.setPrivacyMode(PrivacyMode.private);
    await offline.setConnected(false);
    await tester.pumpWidget(HomeMindApp(dataSource: offline));
    await tester.pump();
    expect(find.text('말하기'), findsOneWidget);
    expect(find.text('로컬 안전 모드'), findsOneWidget);
    expect(tester.takeException(), isNull);

    final listening = MockHomeMindDataSource(seedDemoData: true)
      ..simulateVoiceReply = false;
    await tester.pumpWidget(HomeMindApp(
        key: const ValueKey('listening'), dataSource: listening));
    await tester.pump();
    await tester.tap(find.text('말하기'));
    await tester.pump();
    expect(find.text('듣는 중…'), findsOneWidget);
    expect(find.text('에어컨을 켤까요?'), findsOneWidget);
    expect(tester.takeException(), isNull);
    await tester.pump(const Duration(seconds: 7));
  });
}
