// The phone app (homemind-application) recorded for the demo video's UI scenes, on the same clock as the 7-inch
// clip of each scenario (panel_test.dart). The screens are the real app code in its paired (connected) mode; the
// Mobile API is replaced by [ScriptApi], which answers with what the scenario has reached. Run through record.js.
import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/app/homemind_app.dart';
import 'package:homemind_application/core/api/homemind_api.dart';
import 'package:homemind_application/core/auth/pairing_credentials.dart';
import 'package:homemind_application/models/approval_models.dart';
import 'package:homemind_application/models/hardware_models.dart';
import 'package:homemind_application/state/app_controller.dart';
import 'package:homemind_application/widgets/common_widgets.dart';

import 'recorder.dart';

class ScriptApi extends HomeMindApi {
  ScriptApi() : super(baseUrl: 'https://10.10.10.2:8443');

  HardwareSnapshot dashboard = HardwareSnapshot(
    connected: true,
    updatedAt: '',
    devices: [for (final d in HardwareDemoData.snapshot.devices) d.copyWith(online: true)],
  );
  List<ApprovalItem> approvals = const [];

  @override
  bool get hasToken => true;
  @override
  Future<HardwareSnapshot> getDashboard() async => dashboard;
  @override
  Future<List<ApprovalItem>> getApprovals() async => approvals;
  @override
  Future<void> decideTask(String taskId, {required bool approve}) async =>
      approvals = approvals.where((item) => item.taskId != taskId).toList();

  void setState(String deviceId, String state) => dashboard = dashboard.copyWith(devices: [
        for (final d in dashboard.devices) d.id == deviceId ? d.copyWith(state: state) : d,
      ]);
}

Finder navTab(String label) => find.descendant(of: find.byType(NavigationBar), matching: find.text(label));

final aircon = ApprovalDemoData.items[0];
final door = ApprovalDemoData.items[1];

void main() {
  setUpAll(() async {
    final roboto = [for (final w in ['regular', 'medium', 'bold', 'black']) materialFont('roboto-$w.ttf')];
    await loadFonts({
      'Roboto': roboto,
      'Noto Sans KR': [notoKr],
      'MaterialIcons': [materialFont('materialicons-regular.otf')],
    });
  });

  Future<void> clip(WidgetTester tester, String name, double seconds,
      List<Cue> Function(ScriptApi api, AppController app) cues) async {
    tester.view.physicalSize = const Size(780, 1688);  // 390x844 logical, portrait
    tester.view.devicePixelRatio = 2;
    tester.view.padding = const FakeViewPadding(top: 48);
    addTearDown(tester.view.reset);
    debugDisableShadows = false;
    final api = ScriptApi();
    final app = AppController(api: api, credentials: MemoryCredentialStore())..liveConnected = true;
    await Recorder(tester).record(
        'phone-$name',
        seconds,
        HomeMindApp(controller: app, autoConnect: false, initialMode: HomeMindEntryMode.connected),
        [
          Cue(0, (_) async {
            await app.refresh(silent: true);
            await app.refreshApprovals();
          }),
          ...cues(api, app),
        ],
        pixelRatio: 2);
    debugDisableShadows = true;
  }

  Cue arrive(double t, ScriptApi api, AppController app, List<ApprovalItem> items) => Cue(t, (_) async {
        api.approvals = items;
        await app.refreshApprovals();
      });
  Cue state(double t, ScriptApi api, AppController app, String deviceId, String value) => Cue(t, (_) async {
        api.setState(deviceId, value);
        await app.refresh(silent: true);
      });

  testWidgets('hidden: approval request → 보기 → 승인 → 에어컨 표시 ON', (tester) async {
    await clip(tester, 'hidden', 12, (api, app) => [
          arrive(3.1, api, app, [aircon]),
          Cue(4.6, (rec) => rec.tapText('보기')),
          Cue(5.8, (rec) => rec.tap(find.widgetWithText(PrimaryActionButton, '승인'))),
          state(8.2, api, app, 'LED-AIRCON-01', 'ON'),
          Cue(9.0, (rec) => rec.tap(navTab('장치'))),
        ]);
  });

  testWidgets('doors: 현관문 request cannot be approved on the phone; it goes once the 7-inch screen approves',
      (tester) async {
    await clip(tester, 'doors', 15.5, (api, app) => [
          state(4.4, api, app, 'SERVO-WINDOW-01', 'OPEN'),
          arrive(7.3, api, app, [door]),
          Cue(8.0, (rec) => rec.tapText('보기')),
          arrive(10.9, api, app, const []),
          state(12.8, api, app, 'SERVO-DOOR-01', 'OPEN'),
          Cue(13.4, (rec) => rec.tap(navTab('장치'))),
        ]);
  });
}
