import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';

void _setPiDisplay(WidgetTester tester) {
  tester.view.physicalSize = const Size(1024, 600);
  tester.view.devicePixelRatio = 1;
}

void main() {
  testWidgets('uses the document bottom navigation at 1024x600',
      (tester) async {
    _setPiDisplay(tester);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(HomeMindApp(dataSource: MockHomeMindDataSource()));
    await tester.pump();

    expect(find.text('HomeMind'), findsOneWidget);
    expect(find.text('지금 HomeMind가 이해한 상황'), findsOneWidget);
    expect(find.text('홈'), findsOneWidget);
    expect(find.text('Task'), findsOneWidget);
    expect(find.text('기기'), findsOneWidget);
    expect(find.text('개인정보'), findsOneWidget);
    expect(find.text('더보기'), findsOneWidget);

    await tester.tap(find.byTooltip('프로그램 종료'));
    await tester.pumpAndSettle();
    expect(find.text('HomeMind 종료'), findsOneWidget);
    await tester.tap(find.text('취소'));
    await tester.pumpAndSettle();
  });

  testWidgets('moves developer scenarios under More', (tester) async {
    _setPiDisplay(tester);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    await tester.pumpWidget(HomeMindApp(dataSource: MockHomeMindDataSource()));
    await tester.pump();
    expect(find.text('1. 귀가 감지'), findsNothing);

    await tester.tap(find.text('더보기'));
    await tester.pumpAndSettle();
    expect(find.text('시나리오 시험'), findsOneWidget);
    await tester.tap(find.text('시나리오 시험'));
    await tester.pumpAndSettle();
    expect(find.text('1. 귀가 감지'), findsOneWidget);
    expect(find.text('2. 취침 제안'), findsOneWidget);
    expect(find.text('3. 야간 이탈'), findsOneWidget);
    expect(find.text('명령 시간초과'), findsOneWidget);
  });

  testWidgets('shows task failure, privacy and system pages', (tester) async {
    _setPiDisplay(tester);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final source = MockHomeMindDataSource();
    await source.simulateCommandTimeout();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    await tester.tap(find.text('Task'));
    await tester.pumpAndSettle();
    expect(find.text('기기 응답 없음'), findsOneWidget);

    await tester.tap(find.text('개인정보'));
    await tester.pumpAndSettle();
    expect(find.text('현재 개인정보 모드'), findsOneWidget);
    expect(find.textContaining('영상 저장 안 함'), findsWidgets);
    await tester.tap(find.byType(Switch).first);
    await tester.pumpAndSettle();
    expect(source.current.privacy.mode, PrivacyMode.cameraBlocked);
    expect(source.current.privacy.cameraAllowed, isFalse);

    await tester.tap(find.text('더보기'));
    await tester.pumpAndSettle();
    await tester.tap(find.text('시스템 상태'));
    await tester.pumpAndSettle();
    expect(find.text('Pi4 Core'), findsOneWidget);
    expect(find.textContaining('자동 삭제 안 함'), findsOneWidget);
    expect(find.text('수집 대기'), findsWidgets);
  });

  testWidgets('P0 alert takes over the full screen until ACK', (tester) async {
    _setPiDisplay(tester);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final source = MockHomeMindDataSource();
    await source.triggerSafetyAlert();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    expect(find.text('P0 긴급 안전 경고'), findsOneWidget);
    expect(find.text('홈'), findsNothing);
    await tester.tap(find.text('상황을 확인했습니다'));
    await tester.pumpAndSettle();
    expect(find.text('홈'), findsOneWidget);
  });

  testWidgets('offline mode retains the latest snapshot and explains limits',
      (tester) async {
    _setPiDisplay(tester);
    addTearDown(tester.view.resetPhysicalSize);
    addTearDown(tester.view.resetDevicePixelRatio);

    final source = MockHomeMindDataSource();
    await source.setConnected(false);
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();

    expect(find.textContaining('로컬 안전 모드로 동작 중'), findsOneWidget);
    await tester.tap(find.textContaining('로컬 안전 모드로 동작 중'));
    await tester.pumpAndSettle();
    expect(find.text('오프라인 안전 모드'), findsOneWidget);
    expect(find.text('계속 사용할 수 있어요'), findsOneWidget);
    expect(find.text('잠시 제한돼요'), findsOneWidget);
  });
}
