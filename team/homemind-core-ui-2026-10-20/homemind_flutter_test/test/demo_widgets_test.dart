import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';

void _setDisplay(WidgetTester tester, [Size size = const Size(1024, 600)]) {
  tester.view.physicalSize = size;
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

void main() {
  testWidgets('AI proposal card asks in Korean and 실행 approves it',
      (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource(seedDemoData: true);
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    expect(find.text('AI 제안'), findsWidgets);
    expect(find.text('에어컨을 켤까요?'), findsOneWidget);
    expect(find.text('실행'), findsOneWidget);
    expect(find.text('취소'), findsOneWidget);

    await tester.tap(find.text('실행'));
    await _settle(tester);
    final aircon =
        source.current.tasks.firstWhere((task) => task.title == '에어컨 켜기');
    expect(aircon.state, HomeTaskState.completed);
    expect(find.text('에어컨을 켤까요?'), findsNothing);
    expect(
        source.current.devices
            .firstWhere((device) => device.id == 'LED-AIRCON-01')
            .active,
        isTrue);
  });

  testWidgets('취소 rejects the AI proposal', (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource(seedDemoData: true);
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    await tester.tap(find.text('취소'));
    await _settle(tester);
    expect(
        source.current.tasks.firstWhere((task) => task.title == '에어컨 켜기').state,
        HomeTaskState.rejected);
  });

  testWidgets('quiet banner shows the current activity context',
      (tester) async {
    _setDisplay(tester);
    await tester.pumpWidget(
        HomeMindApp(dataSource: MockHomeMindDataSource(seedDemoData: true)));
    await tester.pump();

    expect(find.textContaining('조용 모드 켜짐 · 회의 중'), findsOneWidget);
    expect(find.text('에어컨 저소음 표시'), findsOneWidget);
    expect(find.text('상황 감지'), findsWidgets);
  });

  testWidgets('quiet mode demo turns the banner on and off', (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();
    expect(find.textContaining('조용 모드 켜짐'), findsNothing);

    source.demoMeetingStarted();
    await _settle(tester);
    expect(find.textContaining('조용 모드 켜짐 · 회의 중'), findsOneWidget);
    expect(
        source.current.devices
            .firstWhere((device) => device.id == 'LED-AIRCON-01')
            .displayValue,
        '깜빡임');

    source.demoMeetingEnded();
    await _settle(tester);
    expect(find.textContaining('조용 모드 켜짐'), findsNothing);
    expect(source.current.quietMode.active, isFalse);
  });

  testWidgets('clarify question appears and can be closed', (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    await source.demoAiClarify();
    await tester.pump();
    expect(find.text('HomeMind가 물어봐요'), findsOneWidget);
    expect(find.text('어느 쪽을 열까요? 현관문인가요, 창문인가요?'), findsOneWidget);

    await tester.tap(find.byTooltip('메시지 닫기'));
    await tester.pump();
    expect(find.text('HomeMind가 물어봐요'), findsNothing);
  });

  testWidgets('task list shows origin badges and Korean rejection reason',
      (tester) async {
    _setDisplay(tester);
    await tester.pumpWidget(
        HomeMindApp(dataSource: MockHomeMindDataSource(seedDemoData: true)));
    await tester.pump();

    await tester.tap(find.text('Task'));
    await tester.pumpAndSettle();
    expect(find.text('거실 조명 켜기'), findsOneWidget);
    expect(find.text('야간 이동 조명 켜기'), findsOneWidget);
    expect(find.text('창문 닫기'), findsOneWidget);
    expect(find.text('조용 모드 켜기'), findsOneWidget);
    expect(find.text('음성 명령'), findsWidgets);
    expect(find.text('휴대폰'), findsOneWidget);
    expect(find.text('상황 감지'), findsWidgets);
    expect(find.text('미배선 기기'), findsOneWidget);

    await tester.tap(find.text('야간 이동 조명 켜기'));
    await tester.pumpAndSettle();
    expect(find.textContaining('아직 배선되지 않아'), findsOneWidget);
    expect(find.textContaining('DEVICE_NOT_WIRED'), findsOneWidget);
  });

  testWidgets('device page lists only what is in the model', (tester) async {
    _setDisplay(tester);
    await tester.pumpWidget(HomeMindApp(dataSource: MockHomeMindDataSource()));
    await tester.pump();

    await tester.tap(find.text('기기'));
    await tester.pumpAndSettle();
    // registry v4: devices not in the model and the doll axis keep their IDs but are not listed
    for (final hidden in const ['침대 커튼', 'TV 표시등', '세탁기 표시등', '화장실 문', 'X축 이동 모터']) {
      expect(find.text(hidden), findsNothing);
    }
    expect(find.text('미배선 · 명령은 거절돼요'), findsNothing);
    await tester.ensureVisible(find.text('창문').first);
    await tester.pumpAndSettle();
    await tester.tap(find.text('창문').first);
    await tester.pumpAndSettle();
    expect(find.text('열기/닫기 모터 · GPIO21'), findsOneWidget);
    expect(find.text('배선됨 (전장 확정안)'), findsOneWidget);
  });

  testWidgets('entrance door voice request waits for approval',
      (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    await source.demoEntranceDoorRequest();
    await tester.pump();
    expect(find.text('확인이 필요해요'), findsWidgets);
    expect(find.text('현관문을 열까요?'), findsOneWidget);
    expect(find.text('음성 명령'), findsOneWidget);
  });

  testWidgets('demo tools list the new task kinds', (tester) async {
    _setDisplay(tester);
    await tester.pumpWidget(HomeMindApp(dataSource: MockHomeMindDataSource()));
    await tester.pump();

    await tester.tap(find.text('더보기'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('시나리오 시험'));
    await tester.pumpAndSettle();
    expect(find.text('AI 제안 (실행/취소)'), findsOneWidget);
    expect(find.text('회의 감지 → 조용 모드'), findsOneWidget);
    expect(find.text('미배선 기기 요청 (거절)'), findsOneWidget);
  });

  testWidgets('busiest home screen fits 1024x600 without overflow',
      (tester) async {
    _setDisplay(tester);
    final source = MockHomeMindDataSource(seedDemoData: true);
    await source.demoAiClarify();
    await source.setConnected(false);
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    expect(find.textContaining('로컬 안전 모드로 동작 중'), findsOneWidget);
    expect(find.textContaining('조용 모드 켜짐'), findsOneWidget);
    expect(find.text('HomeMind가 물어봐요'), findsOneWidget);
    expect(find.text('에어컨을 켤까요?'), findsOneWidget);
    expect(tester.takeException(), isNull);
  });

  test('kiosk frame: 7-inch scale, window shape, no black bars', () {
    expect(kioskFrameSize(const Size(1024, 600)), kHomeMindScreenSize);
    expect(kioskFrameSize(const Size(2048, 1200)), kHomeMindScreenSize);
    final fullHd = kioskFrameSize(const Size(1920, 1080));
    expect(fullHd.height, 600);
    expect(fullHd.width, closeTo(1066.7, 0.1));
    final narrow = kioskFrameSize(const Size(1280, 1024));
    expect(narrow.width, 1024);
    expect(narrow.height, closeTo(819.2, 0.1));
    expect(kioskFrameSize(Size.zero), kHomeMindScreenSize);
  });

  testWidgets('kiosk frame keeps the 7-inch layout and fills a larger window',
      (tester) async {
    _setDisplay(tester, const Size(1600, 900));
    await tester.pumpWidget(HomeMindApp(
        kiosk: true, dataSource: MockHomeMindDataSource(seedDemoData: true)));
    await tester.pump();

    expect(find.text('HomeMind'), findsOneWidget);
    final frame = tester.getSize(find.byType(Scaffold).first);
    expect(frame.height, 600);
    expect(frame.width, closeTo(1066.7, 0.1));
    // Scaled up to the whole window (1.5x here), not left at 1:1 in the middle.
    final shown = tester.getRect(find.byType(Scaffold).first);
    expect(shown.left, closeTo(0, 0.5));
    expect(shown.top, closeTo(0, 0.5));
    expect(shown.width, closeTo(1600, 0.5));
    expect(shown.height, closeTo(900, 0.5));
    await tester.tap(find.text('취소'));
    await _settle(tester);
    expect(find.text('에어컨을 켤까요?'), findsNothing);
  });

  testWidgets('edge guard keeps the app out of the Atlas edge-swipe zones',
      (tester) async {
    _setDisplay(tester, const Size(1920, 1080));
    await tester.pumpWidget(HomeMindApp(
        kiosk: true,
        edgeGuard: true,
        dataSource: MockHomeMindDataSource(seedDemoData: true)));
    await tester.pump();

    // A tap 40 px above the bottom closed the app on the real Pi5-01, 55 px did not.
    final shown = tester.getRect(find.byType(Scaffold).first);
    expect(shown.top, closeTo(1080 * kEdgeGuard.top, 0.5));
    expect(shown.bottom, closeTo(1080 * (1 - kEdgeGuard.bottom), 0.5));
    expect(1080 - shown.bottom, greaterThan(55));
    expect(shown.top, closeTo(1080 - shown.bottom, 0.5)); // same gap top and bottom
    expect(shown.left, closeTo(0, 0.5));
    expect(shown.width, closeTo(1920, 0.5));
    for (final button in find.byType(InkWell).evaluate()) {
      final rect = tester.getRect(find.byWidget(button.widget));
      if (rect.isEmpty) continue;
      expect(rect.bottom, lessThanOrEqualTo(1080 - 55));
      expect(rect.top, greaterThanOrEqualTo(55));
    }
    expect(find.text('HomeMind'), findsOneWidget);
  });
}
