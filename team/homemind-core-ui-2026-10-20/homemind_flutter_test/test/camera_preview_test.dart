import 'dart:typed_data';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/camera_preview.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/launch_config.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/ui_env_file.dart';

/// 1x1 transparent PNG: enough for Image.memory in a widget test.
final _pixel = Uint8List.fromList([
  0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A, 0x00, 0x00, 0x00, 0x0D, 0x49, 0x48, 0x44, 0x52, //
  0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00, 0x00, 0x1F, 0x15, 0xC4,
  0x89, 0x00, 0x00, 0x00, 0x0A, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9C, 0x63, 0x00, 0x01, 0x00, 0x00,
  0x05, 0x00, 0x01, 0x0D, 0x0A, 0x2D, 0xB4, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4E, 0x44, 0xAE,
  0x42, 0x60, 0x82,
]);

class _FakePreview implements CameraPreviewClient {
  final requested = <String>[];

  @override
  Future<Uint8List> frame(String cameraId) async {
    requested.add(cameraId);
    return _pixel;
  }

  @override
  Future<CameraPreviewStatus> status() async => const CameraPreviewStatus(
      cameraAllowed: true,
      detecting: {'CAM-INDOOR-01': true, 'CAM-OUTDOOR-01': false});

  @override
  void close() {}
}

Future<void> _openCameraPage(WidgetTester tester, Widget app) async {
  tester.view.physicalSize = const Size(1024, 600);
  tester.view.devicePixelRatio = 1;
  addTearDown(tester.view.reset);
  await tester.pumpWidget(app);
  await tester.pump();
  await tester.tap(find.text('더보기').last);
  await tester.pumpAndSettle();
  await tester.tap(find.text('실시간 영상과 인식 상태'));
  // the page polls every 350 ms, so pump a fixed time instead of pumpAndSettle
  for (var i = 0; i < 6; i++) {
    await tester.pump(const Duration(milliseconds: 200));
  }
}

void main() {
  test('HOMEMIND_CAMERA_PREVIEW_URL takes a plain http origin only', () {
    String? url(String value) => HomeMindLaunchConfig.from(
        environment: {'HOMEMIND_CAMERA_PREVIEW_URL': value}).cameraPreviewUrl;
    expect(url('http://10.10.10.4:8091'), 'http://10.10.10.4:8091');
    expect(url(' http://10.10.10.4:8091/ '), 'http://10.10.10.4:8091');
    expect(url('http://10.10.10.4'), 'http://10.10.10.4:80');
    expect(url('https://10.10.10.4:8091'), isNull);
    expect(url('http://user:pw@10.10.10.4:8091'), isNull);
    expect(url('http://10.10.10.4:8091/preview'), isNull);
    expect(url('http://10.10.10.4:8091?x=1'), isNull);
    expect(url('10.10.10.4:8091'), isNull);
    expect(HomeMindLaunchConfig.from().cameraPreviewUrl, isNull);
    expect(parseUiEnv('HOMEMIND_CAMERA_PREVIEW_URL=http://10.10.10.4:8091\n'),
        {'HOMEMIND_CAMERA_PREVIEW_URL': 'http://10.10.10.4:8091'});
  });

  test('preview status and errors', () {
    final status = CameraPreviewStatus.fromJson({
      'camera_allowed': false,
      'cameras': [
        {'camera_id': 'CAM-INDOOR-01', 'detecting': true, 'frame_age_ms': null, 'width': 0, 'height': 0},
      ],
    })!;
    expect(status.cameraAllowed, isFalse);
    expect(status.detecting, {'CAM-INDOOR-01': true});
    expect(CameraPreviewStatus.fromJson({'cameras': []}), isNull);
    expect(CameraPreviewStatus.fromJson({'camera_allowed': true, 'cameras': [{'camera_id': 1}]}), isNull);
    expect(cameraPreviewMessage(423), contains('사생활 보호'));
    expect(cameraPreviewMessage(403), contains('allowed_clients'));
    expect(cameraPreviewMessage(500), contains('500'));
    expect(cameraPreviewClientFor(null), isNull);
  });

  testWidgets('더보기 → 카메라 shows both cameras live and their recognition state', (tester) async {
    final preview = _FakePreview();
    await _openCameraPage(
        tester, HomeMindApp(dataSource: MockHomeMindDataSource(), cameraPreview: preview));
    expect(find.text('카메라 사용 중 · 집 안 공유기로만 · 저장 안 함'), findsOneWidget);
    expect(find.bySemanticsLabel('방 카메라 실시간 영상'), findsOneWidget);
    expect(find.bySemanticsLabel('현관 카메라 실시간 영상'), findsOneWidget);
    expect(find.text('인식 중: 인형 위치 판단'), findsOneWidget);
    expect(find.text('빈 장면 촬영 전: 영상만, 인식 안 함'), findsOneWidget);
    expect(preview.requested.toSet(), {'CAM-INDOOR-01', 'CAM-OUTDOOR-01'});
    expect(preview.requested.length, greaterThan(2), reason: 'it keeps polling');

    await tester.tap(find.text('영상 멈춤'));
    await tester.pump();
    final asked = preview.requested.length;
    await tester.pump(const Duration(seconds: 2));
    expect(preview.requested.length, asked, reason: 'paused: no more requests');
    expect(find.bySemanticsLabel('방 카메라 실시간 영상'), findsNothing);
    expect(find.text('영상 멈춤'), findsOneWidget);  // the banner
    await tester.pumpWidget(const SizedBox());  // stop the page's timers
  });

  testWidgets('the privacy mode stops the camera page from asking at all', (tester) async {
    final source = MockHomeMindDataSource();
    await source.setPrivacyMode(PrivacyMode.cameraBlocked);
    final preview = _FakePreview();
    await _openCameraPage(tester, HomeMindApp(dataSource: source, cameraPreview: preview));
    expect(find.text('사생활 보호 모드: 카메라 영상 꺼짐'), findsOneWidget);
    expect(preview.requested, isEmpty);
    await tester.pumpWidget(const SizedBox());
  });

  testWidgets('without a preview address the page says so', (tester) async {
    await _openCameraPage(tester, HomeMindApp(dataSource: MockHomeMindDataSource()));
    expect(find.text('영상 연결 안 됨'), findsOneWidget);
    expect(find.textContaining('HOMEMIND_CAMERA_PREVIEW_URL'), findsOneWidget);
    await tester.pumpWidget(const SizedBox());
  });
}
