import 'package:flutter/foundation.dart' show kIsWeb;

import 'launch_environment.dart';
import 'mqtt_auth.dart';

/// How the UI starts. Values come from `--dart-define`; a native build (Pi5
/// eLinux bundle, Windows) may override them with `HOMEMIND_*` process
/// environment variables, and on web the page URL overrides both, so one
/// build serves every rehearsal:
///
///   http://127.0.0.1:8765/?broker=ws://10.10.10.2:9001&kiosk=1
///   http://127.0.0.1:8765/?mode=mock&seed=1
///   HOMEMIND_PI4_HOST=host.docker.internal ./homemind_flutter_test -b .
///
/// Only connection settings go here. An MQTT password is never accepted as a
/// value; native builds read it from `HOMEMIND_MQTT_PASSWORD_FILE` ([MqttAuthConfig]).
class HomeMindLaunchConfig {
  const HomeMindLaunchConfig({
    this.mockData = false,
    this.seedDemoData = false,
    this.kiosk = false,
    this.edgeGuard = false,
    this.host = '10.10.10.2',
    this.tcpPort = 1883,
    this.webSocketPort = 9001,
    this.webSocketPath = '',
    this.clientId = defaultClientId,
    this.auth = const MqttAuthConfig(),
    this.cameraPreviewUrl,
  });

  static const defaultClientId = 'homemind-ui-laptop-01';

  final bool mockData;
  final bool seedDemoData;

  /// Fixed 1024×600 frame (7-inch display), scaled to fit the window.
  final bool kiosk;

  /// Keep buttons out of the Atlas window manager's edge-swipe zones
  /// (HOMEMIND_EDGE_GUARD; on by default when Atlas starts the UI as an app).
  final bool edgeGuard;
  final String host;
  final int tcpPort;
  final int webSocketPort;
  final String webSocketPath;

  /// MQTT `client_id` in state requests, ui actions and voice/listen.
  final String clientId;

  /// Native MQTT login (user name + password file); anonymous on web.
  final MqttAuthConfig auth;

  /// `http://<NODE-RPI5-02>:8091`: the camera preview of the vision service on the home router
  /// (HOMEMIND_CAMERA_PREVIEW_URL). Null: the camera page shows no video.
  final String? cameraPreviewUrl;

  /// An `http://host[:port]` origin, or null (no https, user info, path or query).
  static String? previewOrigin(String? value) {
    final uri = value == null ? null : Uri.tryParse(value.trim());
    if (uri == null ||
        uri.scheme != 'http' ||
        uri.host.isEmpty ||
        uri.userInfo.isNotEmpty ||
        (uri.path.isNotEmpty && uri.path != '/') ||
        uri.hasQuery ||
        uri.hasFragment) {
      return null;
    }
    return 'http://${uri.host}:${uri.port}';
  }

  String get webSocketUrl => 'ws://$host:$webSocketPort$webSocketPath';

  static const _defineMode =
      String.fromEnvironment('HOMEMIND_DATA_MODE', defaultValue: 'mqtt');
  static const _defineHost =
      String.fromEnvironment('HOMEMIND_PI4_HOST', defaultValue: '10.10.10.2');
  static const _defineTcpPort =
      int.fromEnvironment('HOMEMIND_MQTT_TCP_PORT', defaultValue: 1883);
  static const _defineWsPort =
      int.fromEnvironment('HOMEMIND_MQTT_WS_PORT', defaultValue: 9001);
  static const _defineWsUrl = String.fromEnvironment('HOMEMIND_MQTT_WS_URL');
  static const _defineKiosk = bool.fromEnvironment('HOMEMIND_KIOSK');
  static const _defineSeed = bool.fromEnvironment('HOMEMIND_DEMO_SEED');
  static const _defineClientId = String.fromEnvironment('HOMEMIND_CLIENT_ID',
      defaultValue: defaultClientId);
  static const _defineUsername =
      String.fromEnvironment('HOMEMIND_MQTT_USERNAME');
  static const _definePasswordFile =
      String.fromEnvironment('HOMEMIND_MQTT_PASSWORD_FILE');
  static const _definePasswordGiven =
      bool.hasEnvironment('HOMEMIND_MQTT_PASSWORD');

  static final _hostPattern = RegExp(r'^[A-Za-z0-9.-]{1,253}$');
  static final _clientIdPattern = RegExp(r'^[A-Za-z0-9_-]{1,64}$');

  /// Build-time defines, then native `HOMEMIND_*` environment variables, then
  /// the URL query (`mode`, `broker`, `kiosk`, `seed`).
  factory HomeMindLaunchConfig.resolve(Map<String, String> query) {
    final environment = launchEnvironment();
    return HomeMindLaunchConfig.from(
        query: query,
        environment: environment,
        mode: _defineMode,
        host: _defineHost,
        tcpPort: _defineTcpPort,
        webSocketPort: _defineWsPort,
        webSocketUrl: _defineWsUrl,
        kiosk: _defineKiosk,
        seed: _defineSeed,
        clientId: _defineClientId,
        auth: MqttAuthConfig.from(
          environment: environment,
          username: _defineUsername,
          passwordFile: _definePasswordFile,
          passwordValueGiven: _definePasswordGiven,
          web: kIsWeb,
        ));
  }

  factory HomeMindLaunchConfig.from({
    Map<String, String> query = const {},
    Map<String, String> environment = const {},
    String mode = 'mqtt',
    String host = '10.10.10.2',
    int tcpPort = 1883,
    int webSocketPort = 9001,
    String webSocketUrl = '',
    bool kiosk = false,
    bool seed = false,
    String clientId = defaultClientId,
    MqttAuthConfig auth = const MqttAuthConfig(),
  }) {
    bool flag(String? value, bool fallback) => value == null
        ? fallback
        : const {'1', 'true', 'yes', 'on'}.contains(value.toLowerCase());

    // Native process environment (systemd unit, docker -e) over the defines.
    final envHost = environment['HOMEMIND_PI4_HOST'];
    if (envHost != null && _hostPattern.hasMatch(envHost)) host = envHost;
    final envPort = int.tryParse(environment['HOMEMIND_MQTT_TCP_PORT'] ?? '');
    if (envPort != null && envPort > 0 && envPort < 65536) tcpPort = envPort;
    final envClientId = environment['HOMEMIND_CLIENT_ID'];
    if (envClientId != null) clientId = envClientId;
    if (!_clientIdPattern.hasMatch(clientId)) clientId = defaultClientId;
    kiosk = flag(environment['HOMEMIND_KIOSK'], kiosk);
    mode = environment['HOMEMIND_DATA_MODE'] ?? mode;

    var resolvedHost = host;
    var resolvedPort = webSocketPort;
    var resolvedPath = '';
    final url = query['broker'] ?? webSocketUrl;
    final parsed = url.isEmpty ? null : Uri.tryParse(url);
    if (parsed != null &&
        parsed.scheme == 'ws' &&
        parsed.host.isNotEmpty &&
        parsed.userInfo.isEmpty) {
      resolvedHost = parsed.host;
      resolvedPort = parsed.hasPort ? parsed.port : 9001;
      resolvedPath = parsed.path == '/' ? '' : parsed.path;
    }
    final resolvedMode = query['mode'] ?? mode;
    return HomeMindLaunchConfig(
      mockData: resolvedMode == 'mock',
      seedDemoData: flag(query['seed'], seed),
      kiosk: flag(query['kiosk'], kiosk),
      edgeGuard: flag(environment['HOMEMIND_EDGE_GUARD'], false),
      host: resolvedHost,
      tcpPort: tcpPort,
      webSocketPort: resolvedPort,
      webSocketPath: resolvedPath,
      clientId: clientId,
      auth: auth,
      cameraPreviewUrl:
          previewOrigin(environment['HOMEMIND_CAMERA_PREVIEW_URL']),
    );
  }
}
