// Frame-by-frame recording of a real HomeMind screen under the flutter_test clock: every frame advances the
// virtual clock by exactly 1/30 s, so taps, animations and state changes land on the frame the video scene
// expects. record.js copies this file into a temporary copy of each app (the apps themselves are not changed).
//
// Output per clip: <out>/<clip>/<n>.png (only frames that differ from the previous one) and
// <out>/<clip>/frames.json {fps, width, height, frames: [file index for each video frame]}.
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';
import 'dart:ui' as ui;

import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';
import 'package:flutter/rendering.dart';
import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';

const fps = 30;
const outDir = String.fromEnvironment('HM_UI_OUT');
const flutterRoot = String.fromEnvironment('HM_FLUTTER_ROOT');

/// The 7-inch UI's own font file (the phone falls back to it for Korean, like the Noto font on Android).
const notoKr = String.fromEnvironment('HM_NOTO_KR');

/// Something that happens at [at] seconds of the clip: a state change or a tap.
class Cue {
  const Cue(this.at, this.run);
  final double at;
  final Future<void> Function(Recorder rec) run;
}

/// Registers the fonts the screens use (the test engine knows none by default).
Future<void> loadFonts(Map<String, List<String>> families) async {
  for (final entry in families.entries) {
    final loader = FontLoader(entry.key);
    for (final path in entry.value) {
      loader.addFont(Future.value(ByteData.sublistView(File(path).readAsBytesSync())));
    }
    await loader.load();
  }
}

String materialFont(String name) => '$flutterRoot/bin/cache/artifacts/material_fonts/$name';

class Recorder {
  Recorder(this.tester);
  final WidgetTester tester;
  final _boundary = GlobalKey();
  final _clock = ValueNotifier<double>(0);
  final _touches = <(Offset, double)>[];

  double get now => _clock.value;

  /// Taps the centre of the first widget [finder] matches and shows a touch mark there.
  Future<void> tap(Finder finder) async {
    final at = tester.getCenter(finder.first);
    _touches.add((at, now));
    await tester.tapAt(at);
  }

  Future<void> tapText(String text) => tap(find.text(text));

  /// Records [seconds] of [app] at [fps], running each cue on the first frame at or after its time.
  Future<void> record(String name, double seconds, Widget app, List<Cue> cues, {double pixelRatio = 1}) async {
    final dir = Directory('$outDir/$name');
    if (dir.existsSync()) dir.deleteSync(recursive: true);
    dir.createSync(recursive: true);
    await tester.pumpWidget(RepaintBoundary(
      key: _boundary,
      child: Stack(textDirection: TextDirection.ltr, children: [
        Positioned.fill(child: app),
        Positioned.fill(child: IgnorePointer(child: _TouchMarks(clock: _clock, touches: _touches))),
      ]),
    ));
    final sorted = [...cues]..sort((a, b) => a.at.compareTo(b.at));
    final frames = <int>[];
    Uint8List? last;
    var files = 0, next = 0, width = 0, height = 0;
    final total = (seconds * fps).round();
    for (var i = 0; i < total; i++) {
      _clock.value = i / fps;
      while (next < sorted.length && sorted[next].at <= _clock.value + 1e-6) {
        await sorted[next++].run(this);
      }
      await tester.pump(i == 0 ? Duration.zero : const Duration(microseconds: 1000000 ~/ fps));
      final (png, w, h) = await _capture(pixelRatio);
      width = w;
      height = h;
      if (last == null || !listEquals(last, png)) {
        File('${dir.path}/$files.png').writeAsBytesSync(png);
        files++;
        last = png;
      }
      frames.add(files - 1);
    }
    File('${dir.path}/frames.json').writeAsStringSync(
        jsonEncode({'fps': fps, 'seconds': seconds, 'width': width, 'height': height, 'frames': frames}));
    debugPrint('$name: $total frames, $files distinct, ${width}x$height');
    // let timers the screens started (snackbars, clocks) finish before the next clip
    await tester.pumpWidget(const SizedBox());
    await tester.pump(const Duration(seconds: 10));
    _touches.clear();
  }

  Future<(Uint8List, int, int)> _capture(double pixelRatio) async {
    final boundary = _boundary.currentContext!.findRenderObject()! as RenderRepaintBoundary;
    return (await tester.runAsync(() async {
      final image = await boundary.toImage(pixelRatio: pixelRatio);
      final data = await image.toByteData(format: ui.ImageByteFormat.png);
      final result = (data!.buffer.asUint8List(), image.width, image.height);
      image.dispose();
      return result;
    }))!;
  }
}

/// A finger mark where a tap happened: a filled dot that grows and fades over 0.6 s.
class _TouchMarks extends StatelessWidget {
  const _TouchMarks({required this.clock, required this.touches});
  final ValueNotifier<double> clock;
  final List<(Offset, double)> touches;

  @override
  Widget build(BuildContext context) => ValueListenableBuilder<double>(
        valueListenable: clock,
        builder: (context, now, _) => CustomPaint(painter: _TouchPainter(now, touches), size: Size.infinite),
      );
}

class _TouchPainter extends CustomPainter {
  _TouchPainter(this.now, this.touches);
  final double now;
  final List<(Offset, double)> touches;

  @override
  void paint(Canvas canvas, Size size) {
    for (final (at, t0) in touches) {
      final u = (now - t0) / 0.6;
      if (u < 0 || u > 1) continue;
      final fade = 1 - u;
      canvas.drawCircle(at, 22 + 14 * u, Paint()..color = Colors.white.withValues(alpha: 0.45 * fade));
      canvas.drawCircle(at, 22 + 14 * u,
          Paint()
            ..style = PaintingStyle.stroke
            ..strokeWidth = 3
            ..color = Colors.white.withValues(alpha: 0.9 * fade));
    }
  }

  @override
  bool shouldRepaint(_TouchPainter old) => true;
}
