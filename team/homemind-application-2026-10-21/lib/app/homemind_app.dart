import 'package:flutter/material.dart';

import '../core/theme/homemind_theme.dart';
import '../screens/app_shell.dart';
import '../screens/pairing_screen.dart';
import '../state/app_controller.dart';

enum HomeMindEntryMode { pairing, demo, connected }

class HomeMindApp extends StatefulWidget {
  const HomeMindApp({
    super.key,
    this.controller,
    this.autoConnect = true,
    this.initialMode = HomeMindEntryMode.pairing,
  });

  final AppController? controller;
  final bool autoConnect;
  final HomeMindEntryMode initialMode;

  @override
  State<HomeMindApp> createState() => _HomeMindAppState();
}

class _HomeMindAppState extends State<HomeMindApp> {
  final _navigatorKey = GlobalKey<NavigatorState>();
  late final AppController _controller;
  late final bool _ownsController;
  late HomeMindEntryMode _mode;
  bool _restoring = false;

  @override
  void initState() {
    super.initState();
    _ownsController = widget.controller == null;
    _controller = widget.controller ?? AppController();
    _controller.addListener(_onControllerChanged);
    _mode = widget.initialMode;
    if (_mode == HomeMindEntryMode.connected && widget.autoConnect) {
      WidgetsBinding.instance.addPostFrameCallback((_) {
        _controller.refresh();
        _controller.startAutoRefresh();
      });
    }
    if (_mode == HomeMindEntryMode.pairing && widget.autoConnect) {
      _restoring = true;
      _restoreSession();
    }
  }

  /// A phone paired earlier goes straight to the device screen with its stored token.
  Future<void> _restoreSession() async {
    bool paired = false;
    try {
      paired = await _controller.restoreSession();
    } catch (_) {
      paired = false; // secure storage unavailable: pair again
    }
    if (!mounted) return;
    setState(() => _restoring = false);
    if (paired) {
      _enterConnectedMode();
      _controller.refresh();
    }
  }

  /// 401 from the server (token revoked) or "연결 해제": back to the QR pairing gate.
  /// Paired from the demo's settings page: switch to the live connection.
  void _onControllerChanged() {
    if (_controller.needsPairing) {
      if (_mode == HomeMindEntryMode.pairing) return;
      _navigatorKey.currentState?.popUntil((route) => route.isFirst);
      setState(() => _mode = HomeMindEntryMode.pairing);
      return;
    }
    if (_controller.paired && _mode == HomeMindEntryMode.demo) {
      _enterConnectedMode();
    }
  }

  void _enterDemoMode() {
    setState(() => _mode = HomeMindEntryMode.demo);
  }

  void _enterConnectedMode() {
    _controller.needsPairing = false;
    setState(() => _mode = HomeMindEntryMode.connected);
    if (widget.autoConnect) _controller.startAutoRefresh();
  }

  @override
  void dispose() {
    _controller.removeListener(_onControllerChanged);
    if (_ownsController) _controller.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    return MaterialApp(
      navigatorKey: _navigatorKey,
      title: 'HomeMind',
      debugShowCheckedModeBanner: false,
      theme: buildHomeMindTheme(),
      home: _restoring
          ? const Scaffold(body: Center(child: CircularProgressIndicator()))
          : switch (_mode) {
              HomeMindEntryMode.pairing => PairingScreen(
                controller: _controller,
                initialSetup: true,
                onDemoRequested: _enterDemoMode,
                onDevelopmentConnected: _enterConnectedMode,
                onPaired: _enterConnectedMode,
              ),
              HomeMindEntryMode.demo ||
              HomeMindEntryMode.connected => AppShell(controller: _controller),
            },
    );
  }
}
