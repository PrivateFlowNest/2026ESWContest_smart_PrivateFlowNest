// The 7-inch UI (homemind_flutter_test) recorded for the demo video: one clip per scenario, shown full screen in
// the video's UI scenes (ui-<scenario> in scenes/scene.js) next to the phone clip of the same scenario. Core's task
// updates arrive at scripted times and the taps happen on cue. The screen is the real UI code; only the data is
// scripted (the live MQTT source is replaced by [ScriptSource]), so the context line follows the live source's rule.
// Run through record.js (it copies this into a temporary copy of the app).
import 'dart:async';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/device_catalog.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/task_presentation.dart';
import 'package:homemind_flutter_test/src/voice_listen.dart';

import 'recorder.dart';

/// Shows the snapshots the script hands it; the screen's own requests (approve, feedback, 말하기) go nowhere,
/// core's answers arrive as scripted snapshots at the scene's time.
class ScriptSource extends MockHomeMindDataSource {
  ScriptSource(this._shown);
  HomeSnapshot _shown;
  final _stream = StreamController<HomeSnapshot>.broadcast();

  @override
  bool get isLive => true;
  @override
  Stream<HomeSnapshot> get snapshots => _stream.stream;
  @override
  HomeSnapshot get current => _shown;
  @override
  Future<void> start() async => _stream.add(_shown);
  @override
  Future<void> requestSnapshot() async => _stream.add(_shown);
  @override
  Future<void> actOnTask(String taskId, TaskAction action) async {}
  @override
  Future<bool> requestVoiceListen(VoiceListenAction action) async => true;

  void show(HomeSnapshot next) {
    _shown = next;
    _stream.add(next);
  }

  @override
  Future<void> dispose() async {
    await _stream.close();
    await super.dispose();
  }
}

final _base = MockHomeMindDataSource().current;
var _stamp = DateTime.now();

/// Later calls get later times, so "newest task" comparisons follow the script order.
DateTime _next() => _stamp = _stamp.add(const Duration(seconds: 1));

HomeTask control(String id, String deviceId, String action, Map<String, dynamic> args,
    {required String origin, required String node, String? aiDevice, String? aiAction}) {
  final parameters = <String, dynamic>{
    'target_device_id': deviceId,
    'action': action,
    'parameters': args,
    'origin': origin,
    'ai_device': aiDevice,
    'ai_action': aiAction,
  };
  final text = describeTask('device_control', parameters);
  return HomeTask(
    id: id,
    title: text.title,
    detail: text.detail,
    state: HomeTaskState.ready,
    priority: 50,
    revision: 1,
    intent: 'device_control',
    targetDevice: deviceId,
    confidence: origin == 'llm' ? 0.86 : 0.95,
    origin: origin,
    sourceNodeId: node,
    parameters: parameters,
    updatedAt: _next(),
  );
}

HomeTask scenario(String id, String intent, {String? rule}) {
  final text = describeTask(intent, const {});
  return HomeTask(
    id: id,
    title: text.title,
    detail: text.detail,
    state: HomeTaskState.ready,
    priority: 40,
    revision: 1,
    intent: intent,
    targetDevice: 'HOME-ENV-01',
    confidence: 0.9,
    origin: 'context_rule',
    sourceNodeId: 'NODE-RPI4-01',
    parameters: {'origin': 'context_rule', if (rule != null) 'rule': rule},
    updatedAt: _next(),
  );
}

HomeTask at(HomeTask task, HomeTaskState state) =>
    task.copyWith(state: state, revision: task.revision + 1, updatedAt: _next());

/// A home screen snapshot; the context line is what the live source writes (newest task and its state).
HomeSnapshot home(List<HomeTask> tasks,
    {HomeZone zone = HomeZone.living, ActivityContext? activity, LlmResultInfo? llm, PartialIntentHint? hint}) {
  var devices = buildOfficialDeviceStatuses(online: true);
  for (final task in tasks.reversed) {
    if (task.state == HomeTaskState.completed) devices = applyTaskEffects(devices, task);
  }
  return _base.copyWith(
    zone: zone,
    contextText: tasks.isEmpty ? 'Pi4 연결됨 · 현재 위치 ${zone.label}' : '${tasks.first.title} · ${tasks.first.state.label}',
    tasks: tasks,
    devices: devices,
    connectionDetail: 'Pi4 MQTT',
    activity: activity,
    llmResult: llm,
    partialHint: hint,
    lastUpdated: DateTime.now(),
  );
}

ActivityContext meeting({bool on = true}) => ActivityContext(
    activity: 'MEETING',
    audioContext: 'ZOOM_SPEAKER',
    quietRequired: on,
    transition: 'ACTIVATED',
    zone: 'desk',
    confidence: 0.84,
    observedAt: DateTime.now());

Cue show(double t, ScriptSource source, HomeSnapshot Function() snapshot) =>
    Cue(t, (_) async => source.show(snapshot()));
Cue tap(double t, String text) => Cue(t, (rec) => rec.tapText(text));

Widget panel(ScriptSource source) => HomeMindApp(dataSource: source, kiosk: true);

void main() {
  setUpAll(() async {
    await loadFonts({
      'NotoSansKR': [notoKr],
      'MaterialIcons': [materialFont('materialicons-regular.otf')],
    });
  });

  // 16:9 for the video: the kiosk frame widens the 1024x600 layout to 1067x600 and scales it to the window
  // (the same code path as the Full HD monitor on Pi5-01).
  Future<void> clip(WidgetTester tester, String name, double seconds, HomeSnapshot first,
      List<Cue> Function(ScriptSource source) cues) async {
    tester.view.physicalSize = const Size(1280, 720);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    debugDisableShadows = false;
    final source = ScriptSource(first);
    await Recorder(tester).record('panel-$name', seconds, panel(source), cues(source));
    debugDisableShadows = true;
  }

  // Times are the UI scene's clock. EXAONE takes 15-20 s on the device; the scene cuts that wait (its caption says so).
  testWidgets('hidden: 말하기 → EXAONE proposal → approved on the phone → done', (tester) async {
    final aircon = control('llm-aircon', 'LED-AIRCON-01', 'set_switch', {'on': true},
        origin: 'llm', node: 'NODE-RPI5-01', aiDevice: 'AIR_CONDITIONER', aiAction: 'TURN_ON');
    final llm = LlmResultInfo(
        eventId: 'llm-1',
        status: 'task_requested',
        taskId: aircon.id,
        aiIntent: 'DEVICE_CONTROL',
        aiDevice: 'AIR_CONDITIONER',
        aiAction: 'TURN_ON',
        completedAt: DateTime.now());
    await clip(tester, 'hidden', 12, home([]), (s) => [
          tap(0.6, '말하기'),
          show(3.0, s, () => home([at(aircon, HomeTaskState.proposed)], llm: llm)),
          show(6.4, s, () => home([at(aircon, HomeTaskState.running)], llm: llm)),
          show(8.0, s, () => home([at(aircon, HomeTaskState.completed)], llm: llm)),
        ]);
  });

  testWidgets('quiet: meeting → quiet mode → 상황이 달라요 → off', (tester) async {
    final on = scenario('ctx-quiet-on', 'quiet_mode_on');
    final off = scenario('ctx-quiet-off', 'quiet_mode_off', rule: 'quiet_context_wrong');
    await clip(tester, 'quiet', 11, home([], zone: HomeZone.desk), (s) {
      late HomeTask onDone;
      return [
        show(1.0, s, () => home([], zone: HomeZone.desk, activity: meeting())),
        show(2.2, s, () => home([at(on, HomeTaskState.running)], zone: HomeZone.desk, activity: meeting())),
        show(3.0, s, () => home([onDone = at(on, HomeTaskState.completed)], zone: HomeZone.desk, activity: meeting())),
        tap(6.0, '상황이 달라요'),
        show(7.2, s, () => home([at(off, HomeTaskState.completed), onDone], zone: HomeZone.desk, activity: meeting())),
      ];
    });
  });

  testWidgets('doors: 창문 runs at once; 현관문 waits for 실행 on this screen', (tester) async {
    final window = control('voice-window', 'SERVO-WINDOW-01', 'set_position', {'position_degrees': 90},
        origin: 'voice_parser', node: 'NODE-RPI5-02', aiDevice: 'WINDOW', aiAction: 'OPEN');
    final door = control('voice-door', 'SERVO-DOOR-01', 'set_position', {'position_degrees': 90},
        origin: 'voice_parser', node: 'NODE-RPI5-02', aiDevice: 'ENTRY_DOOR', aiAction: 'OPEN');
    await clip(tester, 'doors', 15.5, home([]), (s) {
      late HomeTask windowDone;
      return [
        tap(0.5, '말하기'),
        show(2.0, s, () => home([at(window, HomeTaskState.ready)])),
        show(2.6, s, () => home([at(window, HomeTaskState.running)])),
        show(4.2, s, () => home([windowDone = at(window, HomeTaskState.completed)])),
        tap(5.6, '말하기'),
        show(7.2, s, () => home([at(door, HomeTaskState.proposed), windowDone])),
        tap(10.4, '실행'),
        show(10.8, s, () => home([at(door, HomeTaskState.running), windowDone])),
        show(12.6, s, () => home([at(door, HomeTaskState.completed), windowDone])),
      ];
    });
  });

  testWidgets('partial: 말하기 → 거실 조명 준비 while speaking → runs after the sentence', (tester) async {
    final light = control('voice-light', 'LED-LIVING-01', 'set_switch', {'on': true},
        origin: 'voice_parser', node: 'NODE-RPI5-02', aiDevice: 'LIGHT', aiAction: 'TURN_ON');
    PartialIntentHint hint() => PartialIntentHint(
        intent: 'LIGHTING', candidateDevices: const ['LED-LIVING-01'], confidence: 0.81, observedAt: DateTime.now());
    await clip(tester, 'partial', 9, home([]), (s) => [
          tap(0.4, '말하기'),
          // the hint repeats while the sentence goes on (the screen drops it 8 s after the last one)
          for (final t in [1.6, 2.4, 3.2]) show(t, s, () => home([], hint: hint())),
          show(4.0, s, () => home([at(light, HomeTaskState.ready)])),
          show(4.3, s, () => home([at(light, HomeTaskState.running)])),
          show(5.8, s, () => home([at(light, HomeTaskState.completed)])),
        ]);
  });
}
