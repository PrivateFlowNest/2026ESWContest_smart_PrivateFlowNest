import 'dart:async';

import 'package:flutter/foundation.dart';
import 'package:flutter/gestures.dart' show PointerDeviceKind;
import 'package:flutter/material.dart';
import 'package:flutter/services.dart';

import 'camera_preview.dart';
import 'device_catalog.dart';
import 'home_mind_data_source.dart';
import 'mock_home_mind_data_source.dart';
import 'models.dart';
import 'mqtt_home_mind_data_source.dart';
import 'phone_pairing.dart';
import 'task_presentation.dart';
import 'voice_listen.dart';

part 'home_mind_pages.dart';
part 'home_mind_detail_pages.dart';
part 'home_mind_widgets.dart';

const _navy = Color(0xFF0E1726);
const _surface = Color(0xFF172233);
const _mint = Color(0xFF63DFC2);
const _amber = Color(0xFFFFBD66);
const _purple = Color(0xFFB69BFF);
const _danger = Color(0xFFFF615C); // P0 화면 전용
const _sky = Color(0xFF7CC4FF);
const _rose = Color(0xFFF2A7D8);

/// 7-inch Pi5 display.
const kHomeMindScreenSize = Size(1024, 600);

class HomeMindApp extends StatelessWidget {
  const HomeMindApp(
      {super.key,
      this.dataSource,
      this.kiosk = false,
      this.edgeGuard = false,
      this.cameraPreview});
  final HomeMindDataSource? dataSource;

  /// Live video of NODE-RPI5-02's cameras on the camera page; null: none.
  final CameraPreviewClient? cameraPreview;

  /// Render the 7-inch layout (scaled to the window) without scrollbars, as on
  /// the 7-inch kiosk display.
  final bool kiosk;

  /// Leave the top and bottom edges free ([kEdgeGuard]).
  final bool edgeGuard;

  @override
  Widget build(BuildContext context) => MaterialApp(
        debugShowCheckedModeBanner: false,
        title: 'HomeMind',
        scrollBehavior: _HomeMindScrollBehavior(showScrollbars: !kiosk),
        builder: kiosk || edgeGuard
            ? (context, child) {
                Widget app = child ?? const SizedBox();
                if (kiosk) app = _KioskFrame(child: app);
                return edgeGuard ? _EdgeGuard(child: app) : app;
              }
            : null,
        theme: ThemeData(
          useMaterial3: true,
          fontFamily: 'NotoSansKR',
          brightness: Brightness.dark,
          scaffoldBackgroundColor: _navy,
          cardColor: _surface,
          colorScheme: ColorScheme.fromSeed(
            seedColor: _mint,
            brightness: Brightness.dark,
            surface: _surface,
          ),
          textTheme: const TextTheme(
            bodyLarge: TextStyle(fontSize: 18),
            titleLarge: TextStyle(fontSize: 24, fontWeight: FontWeight.w800),
          ),
          filledButtonTheme: FilledButtonThemeData(
            style: FilledButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
          outlinedButtonTheme: OutlinedButtonThemeData(
            style: OutlinedButton.styleFrom(minimumSize: const Size(48, 48)),
          ),
        ),
        home: HomeMindDashboard(
          dataSource: dataSource ?? MockHomeMindDataSource(),
          cameraPreview: cameraPreview,
        ),
      );
}

/// Touch and mouse drag both scroll (kiosk touch screen, PC rehearsal).
class _HomeMindScrollBehavior extends MaterialScrollBehavior {
  const _HomeMindScrollBehavior({required this.showScrollbars});
  final bool showScrollbars;

  @override
  Set<PointerDeviceKind> get dragDevices => const {
        PointerDeviceKind.touch,
        PointerDeviceKind.mouse,
        PointerDeviceKind.stylus,
        PointerDeviceKind.trackpad,
      };

  @override
  Widget buildScrollbar(
          BuildContext context, Widget child, ScrollableDetails details) =>
      showScrollbars ? super.buildScrollbar(context, child, details) : child;
}

/// The 7-inch layout for a [window]: 600 logical pixels high (1024 wide) like
/// the 7-inch display, made wider or taller to the window's shape so the frame
/// fills it without black bars (1067×600 on a Full HD monitor).
Size kioskFrameSize(Size window) {
  const base = kHomeMindScreenSize;
  if (window.isEmpty) return base;
  final aspect = window.width / window.height;
  return aspect >= base.aspectRatio
      ? Size(base.height * aspect, base.height)
      : Size(base.width, base.width / aspect);
}

/// Share of the window height kept free at the top and bottom. The Atlas
/// window manager treats a touch that starts in the bottom edge zone as a
/// "BottomToTop" swipe, even a plain tap, and closes the foreground app (real
/// Pi5-01 on 1920×1080, 2026-10-23: a tap 40 px above the bottom closed the UI,
/// 55 px did not; as an "overlay" app it was closed the same way). The top is
/// kept free by the same amount so the layout stays centred (user's choice).
const kEdgeGuard = (top: 0.06, bottom: 0.06);

/// Keeps the app out of the edge-swipe zones, on the app's background colour.
class _EdgeGuard extends StatelessWidget {
  const _EdgeGuard({required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final media = MediaQuery.of(context);
    final height = media.size.height;
    return ColoredBox(
      color: _navy,
      child: Padding(
        padding: EdgeInsets.only(
            top: height * kEdgeGuard.top, bottom: height * kEdgeGuard.bottom),
        child: LayoutBuilder(
          builder: (context, constraints) => MediaQuery(
            data: media.copyWith(
              size: constraints.biggest,
              padding: EdgeInsets.zero,
              viewPadding: EdgeInsets.zero,
            ),
            child: child,
          ),
        ),
      ),
    );
  }
}

/// Lays the app out at the 7-inch scale ([kioskFrameSize]) and scales it to
/// fill the window, so a PC window or a larger monitor shows the same layout
/// as the 7-inch display.
class _KioskFrame extends StatelessWidget {
  const _KioskFrame({required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) {
    final media = MediaQuery.of(context);
    final frame = kioskFrameSize(media.size);
    // SizedBox.expand: FittedBox only scales up under tight constraints. Under
    // Center it kept the frame at 1:1 (real Pi5-01 on a Full HD monitor,
    // 2026-10-23: the 1024x600 layout sat in the middle with wide black bars).
    return ColoredBox(
      color: Colors.black,
      child: SizedBox.expand(
        child: FittedBox(
          fit: BoxFit.contain,
          child: SizedBox.fromSize(
            size: frame,
            child: MediaQuery(
              data: media.copyWith(
                size: frame,
                padding: EdgeInsets.zero,
                viewPadding: EdgeInsets.zero,
                viewInsets: EdgeInsets.zero,
              ),
              child: ClipRect(child: child),
            ),
          ),
        ),
      ),
    );
  }
}

class HomeMindDashboard extends StatefulWidget {
  const HomeMindDashboard(
      {required this.dataSource, this.cameraPreview, super.key});
  final HomeMindDataSource dataSource;
  final CameraPreviewClient? cameraPreview;

  @override
  State<HomeMindDashboard> createState() => _HomeMindDashboardState();
}

class _HomeMindDashboardState extends State<HomeMindDashboard> {
  late HomeSnapshot _snapshot;
  StreamSubscription<HomeSnapshot>? _subscription;
  Timer? _clockTimer;
  int _pageIndex = 0;
  DateTime _now = DateTime.now();
  String? _dismissedLlmEventId;
  final Set<String> _seenProposalIds = {};

  /// Non-null while the 말하기 button shows "듣는 중…".
  VoiceListenWatch? _listenWatch;
  Timer? _listenTimer;

  @override
  void initState() {
    super.initState();
    _snapshot = widget.dataSource.current;
    _rememberProposals(_snapshot);
    _subscription = widget.dataSource.snapshots.listen((snapshot) {
      if (!mounted) return;
      setState(() {
        // A new AI proposal brings the home screen (with 실행/취소) forward.
        if (_rememberProposals(snapshot)) _pageIndex = 0;
        if (_listenWatch?.answeredBy(snapshot) == true) _endListening();
        _snapshot = snapshot;
      });
    });
    _clockTimer = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() => _now = DateTime.now());
    });
    widget.dataSource.start();
  }

  /// Returns true when an AI proposal appeared that was not seen before.
  bool _rememberProposals(HomeSnapshot snapshot) {
    var fresh = false;
    for (final task in snapshot.tasks) {
      if (task.isAiProposal && _seenProposalIds.add(task.id)) fresh = true;
    }
    return fresh;
  }

  void _endListening() {
    _listenTimer?.cancel();
    _listenTimer = null;
    _listenWatch = null;
  }

  /// 말하기 → start; pressed again while listening → cancel.
  Future<void> _toggleTalk() async {
    final source = widget.dataSource;
    if (_listenWatch != null) {
      setState(_endListening);
      await source.requestVoiceListen(VoiceListenAction.cancel);
      return;
    }
    setState(() {
      _listenWatch = VoiceListenWatch(_snapshot);
      _listenTimer = Timer(kVoiceListenFeedback, () {
        if (mounted) setState(_endListening);
      });
    });
    final sent = await source.requestVoiceListen(VoiceListenAction.start);
    if (sent || !mounted) return;
    setState(_endListening);
    ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
        content: Text('Pi4 연결이 끊겨 음성 듣기를 시작하지 못했어요')));
  }

  @override
  void dispose() {
    _listenTimer?.cancel();
    _clockTimer?.cancel();
    _subscription?.cancel();
    widget.dataSource.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    if (_snapshot.safetyAlert) {
      return _SafetyTakeover(
        snapshot: _snapshot,
        onAcknowledge: widget.dataSource.acknowledgeSafetyAlert,
      );
    }
    final pages = <Widget>[
      _HomePage(
        snapshot: _snapshot,
        dataSource: widget.dataSource,
        dismissedLlmEventId: _dismissedLlmEventId,
        onDismissLlm: (eventId) =>
            setState(() => _dismissedLlmEventId = eventId),
      ),
      _TaskPage(snapshot: _snapshot, dataSource: widget.dataSource),
      _DevicePage(snapshot: _snapshot),
      _PrivacyPage(snapshot: _snapshot, dataSource: widget.dataSource),
      _MorePage(
          snapshot: _snapshot,
          dataSource: widget.dataSource,
          cameraPreview: widget.cameraPreview),
    ];
    return Scaffold(
      body: SafeArea(
        child: Column(children: [
          _Header(
              snapshot: _snapshot,
              isLive: widget.dataSource.isLive,
              now: _now,
              listening: _listenWatch != null,
              onTalk: _toggleTalk),
          if (!_snapshot.connected)
            _OfflineBanner(
                snapshot: _snapshot,
                onRetry: widget.dataSource.requestSnapshot),
          if (_snapshot.partialHint?.freshAt(_now) ?? false)
            _PartialHintBanner(hint: _snapshot.partialHint!),
          if (_snapshot.quietMode.active)
            _QuietModeBanner(snapshot: _snapshot, dataSource: widget.dataSource),
          Expanded(
            child: AnimatedSwitcher(
              duration: const Duration(milliseconds: 180),
              child: KeyedSubtree(
                  key: ValueKey(_pageIndex), child: pages[_pageIndex]),
            ),
          ),
          NavigationBar(
            height: 72,
            backgroundColor: const Color(0xFF111C2C),
            indicatorColor: _mint.withValues(alpha: .18),
            selectedIndex: _pageIndex,
            onDestinationSelected: (index) =>
                setState(() => _pageIndex = index),
            destinations: const [
              NavigationDestination(
                  icon: Icon(Icons.home_outlined), label: '홈'),
              NavigationDestination(
                  icon: Icon(Icons.task_alt_outlined), label: 'Task'),
              NavigationDestination(
                  icon: Icon(Icons.devices_other), label: '기기'),
              NavigationDestination(
                  icon: Icon(Icons.privacy_tip_outlined), label: '개인정보'),
              NavigationDestination(
                  icon: Icon(Icons.apps_outlined), label: '더보기'),
            ],
          ),
        ]),
      ),
    );
  }
}

class _Header extends StatelessWidget {
  const _Header(
      {required this.snapshot,
      required this.isLive,
      required this.now,
      this.listening = false,
      this.onTalk});
  final HomeSnapshot snapshot;
  final bool isLive;
  final DateTime now;
  final bool listening;
  final VoidCallback? onTalk;

  Future<void> _exit(BuildContext context) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('HomeMind 종료'),
        content: Text(kIsWeb ? '웹 미리보기는 이 탭을 닫으면 종료됩니다.' : '프로그램을 종료할까요?'),
        actions: [
          TextButton(
              onPressed: () => Navigator.pop(context, false),
              child: const Text('취소')),
          FilledButton(
              onPressed: () => Navigator.pop(context, true),
              child: Text(kIsWeb ? '확인' : '종료')),
        ],
      ),
    );
    if (confirmed == true && !kIsWeb) await SystemNavigator.pop();
  }

  @override
  Widget build(BuildContext context) => Container(
        height: 64,
        padding: const EdgeInsets.symmetric(horizontal: 20),
        color: const Color(0xFF111C2C),
        child: Row(children: [
          const Icon(Icons.home_work_rounded, color: _mint, size: 30),
          const SizedBox(width: 9),
          const Text('HomeMind',
              style: TextStyle(fontSize: 23, fontWeight: FontWeight.w900)),
          const SizedBox(width: 18),
          Text(_clock(now),
              style:
                  const TextStyle(fontSize: 18, fontWeight: FontWeight.w700)),
          const SizedBox(width: 12),
          Expanded(
            child: Row(mainAxisAlignment: MainAxisAlignment.end, children: [
              _TalkButton(
                listening: listening,
                connected: snapshot.connected,
                microphoneAllowed: snapshot.privacy.microphoneAllowed,
                onPressed: onTalk,
              ),
              const SizedBox(width: 10),
              _StatusPill(
                icon:
                    snapshot.connected ? Icons.hub : Icons.cloud_off_outlined,
                label: snapshot.connected
                    ? (isLive ? 'Pi4 Core 연결' : '가상 Core 연결')
                    : '로컬 안전 모드',
                color: snapshot.connected ? _mint : _amber,
              ),
              const SizedBox(width: 8),
              Flexible(
                child: _StatusPill(
                    shrinkable: true,
                    icon: snapshot.privacy.cameraAllowed
                        ? Icons.visibility_outlined
                        : Icons.visibility_off_outlined,
                    label: snapshot.privacy.recording
                        ? '카메라 녹화 중'
                        : '${snapshot.privacy.mode.label} · 저장 안 함',
                    color: _purple),
              ),
              const SizedBox(width: 4),
              IconButton(
                  tooltip: '프로그램 종료',
                  onPressed: () => _exit(context),
                  icon: const Icon(Icons.power_settings_new,
                      color: Colors.white70)),
            ]),
          ),
        ]),
      );
}

class _OfflineBanner extends StatelessWidget {
  const _OfflineBanner({required this.snapshot, required this.onRetry});
  final HomeSnapshot snapshot;
  final Future<void> Function() onRetry;

  @override
  Widget build(BuildContext context) => Material(
        color: const Color(0xFF4A3820),
        child: InkWell(
          onTap: () => _openPage(
              context, _OfflineSafePage(snapshot: snapshot, onRetry: onRetry)),
          child: SizedBox(
            height: 52,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 20),
              child: Row(children: [
                const Icon(Icons.cloud_off_outlined, color: _amber),
                const SizedBox(width: 10),
                const Expanded(
                  child: Text('인터넷 없이 로컬 안전 모드로 동작 중 · 자세히 보기',
                      style: TextStyle(
                          color: _amber, fontWeight: FontWeight.w800)),
                ),
                Text('마지막 갱신 ${_clock(snapshot.lastUpdated)}',
                    style: const TextStyle(color: Colors.white70)),
                const Icon(Icons.chevron_right),
              ]),
            ),
          ),
        ),
      );
}

class _SafetyTakeover extends StatelessWidget {
  const _SafetyTakeover({required this.snapshot, required this.onAcknowledge});
  final HomeSnapshot snapshot;
  final Future<void> Function() onAcknowledge;

  @override
  Widget build(BuildContext context) => Scaffold(
        backgroundColor: const Color(0xFF2B1114),
        body: SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(28),
            child: Column(children: [
              const Row(children: [
                Icon(Icons.warning_amber_rounded, color: _danger, size: 42),
                SizedBox(width: 12),
                Text('P0 긴급 안전 경고',
                    style: TextStyle(
                        color: _danger,
                        fontSize: 30,
                        fontWeight: FontWeight.w900)),
                Spacer(),
                Text('사용자 확인 전까지 이 화면을 유지합니다',
                    style: TextStyle(color: Colors.white70)),
              ]),
              const Spacer(),
              Container(
                width: double.infinity,
                padding: const EdgeInsets.all(28),
                decoration: BoxDecoration(
                  color: const Color(0xFF461C20),
                  borderRadius: BorderRadius.circular(22),
                  border: Border.all(color: _danger, width: 2),
                ),
                child: Column(children: [
                  const Icon(Icons.crisis_alert, color: _danger, size: 58),
                  const SizedBox(height: 10),
                  Text(snapshot.safetyDetail?.summary ?? '안전 위험을 감지했습니다',
                      style: const TextStyle(
                          fontSize: 30, fontWeight: FontWeight.w900)),
                  const SizedBox(height: 8),
                  Text(
                      '${snapshot.safetyDetail?.zone ?? snapshot.zone.label} · ${_clock(snapshot.safetyDetail?.raisedAt ?? snapshot.lastUpdated)}',
                      style:
                          const TextStyle(color: Colors.white70, fontSize: 18)),
                  const SizedBox(height: 12),
                  Text(
                      snapshot.safetyDetail?.automaticActions.isNotEmpty == true
                          ? '자동 실행: ${snapshot.safetyDetail!.automaticActions.join(' · ')}'
                          : '주변과 기기 상태를 직접 확인해 주세요.',
                      textAlign: TextAlign.center,
                      style: const TextStyle(fontSize: 18)),
                ]),
              ),
              const Spacer(),
              Row(children: [
                Expanded(
                  flex: 2,
                  child: FilledButton.icon(
                    style: FilledButton.styleFrom(
                        backgroundColor: _danger,
                        foregroundColor: Colors.white,
                        minimumSize: const Size(48, 58)),
                    onPressed: onAcknowledge,
                    icon: const Icon(Icons.check_circle_outline),
                    label: const Text('상황을 확인했습니다',
                        style: TextStyle(
                            fontSize: 18, fontWeight: FontWeight.w900)),
                  ),
                ),
                const SizedBox(width: 12),
                Expanded(
                  child: OutlinedButton.icon(
                    style: OutlinedButton.styleFrom(
                        minimumSize: const Size(48, 58)),
                    onPressed: () {},
                    icon: const Icon(Icons.call_outlined),
                    label: const Text('긴급 연락'),
                  ),
                ),
              ]),
            ]),
          ),
        ),
      );
}

Future<void> _openPage(BuildContext context, Widget page) =>
    Navigator.of(context).push(MaterialPageRoute<void>(builder: (_) => page));

String _clock(DateTime value) =>
    '${value.hour.toString().padLeft(2, '0')}:${value.minute.toString().padLeft(2, '0')}';

Color _originColor(TaskOrigin origin) => switch (origin) {
      TaskOrigin.voice => _mint,
      TaskOrigin.ai => _purple,
      TaskOrigin.phone => _sky,
      TaskOrigin.screen => _sky,
      TaskOrigin.context => _rose,
      TaskOrigin.unknown => Colors.white54,
    };

IconData _originIcon(TaskOrigin origin) => switch (origin) {
      TaskOrigin.voice => Icons.mic_none,
      TaskOrigin.ai => Icons.auto_awesome,
      TaskOrigin.phone => Icons.smartphone,
      TaskOrigin.screen => Icons.touch_app_outlined,
      TaskOrigin.context => Icons.sensors,
      TaskOrigin.unknown => Icons.help_outline,
    };

Color _taskStateColor(HomeTaskState state) => switch (state) {
      HomeTaskState.completed => _mint,
      HomeTaskState.failed || HomeTaskState.deferred => _amber,
      HomeTaskState.rejected || HomeTaskState.cancelled => Colors.white54,
      _ => _purple,
    };

IconData _taskStateIcon(HomeTaskState state) => switch (state) {
      HomeTaskState.completed => Icons.check,
      HomeTaskState.failed => Icons.warning_amber_rounded,
      HomeTaskState.rejected || HomeTaskState.cancelled => Icons.block,
      HomeTaskState.deferred => Icons.pause,
      HomeTaskState.running => Icons.play_arrow,
      HomeTaskState.ready => Icons.hourglass_top,
      HomeTaskState.proposed => Icons.touch_app_outlined,
    };

String _bytes(int value) {
  if (value <= 0) return '확인 중';
  if (value < 1024 * 1024) return '${(value / 1024).toStringAsFixed(1)} KiB';
  if (value < 1024 * 1024 * 1024) {
    return '${(value / 1024 / 1024).toStringAsFixed(1)} MiB';
  }
  return '${(value / 1024 / 1024 / 1024).toStringAsFixed(2)} GiB';
}
