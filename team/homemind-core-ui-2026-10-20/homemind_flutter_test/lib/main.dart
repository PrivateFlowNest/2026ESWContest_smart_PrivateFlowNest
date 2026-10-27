import 'dart:async';

import 'package:flutter/material.dart';

import 'src/atlas_registration.dart';
import 'src/camera_preview.dart';
import 'src/home_mind_app.dart';
import 'src/launch_config.dart';
import 'src/mock_home_mind_data_source.dart';
import 'src/mqtt_home_mind_data_source.dart';

void main() {
  // First: the Atlas app manager stops an app that has not registered within ~3 s.
  unawaited(registerWithAtlasAppManager());
  final config = HomeMindLaunchConfig.resolve(Uri.base.queryParameters);

  runApp(
    HomeMindApp(
      kiosk: config.kiosk,
      edgeGuard: config.edgeGuard,
      cameraPreview:
          config.mockData ? null : cameraPreviewClientFor(config.cameraPreviewUrl),
      dataSource: config.mockData
          ? MockHomeMindDataSource(seedDemoData: config.seedDemoData)
          : MqttHomeMindDataSource(
              host: config.host,
              tcpPort: config.tcpPort,
              webSocketPort: config.webSocketPort,
              webSocketPath: config.webSocketPath,
              clientId: config.clientId,
              auth: config.auth,
            ),
    ),
  );
  // Window size given by the compositor, for checking the display on a device.
  WidgetsBinding.instance.addPostFrameCallback((_) {
    final view = WidgetsBinding.instance.platformDispatcher.implicitView;
    if (view != null) {
      debugPrint('homemind-ui: window ${view.physicalSize.width.round()}x'
          '${view.physicalSize.height.round()} px, scale ${view.devicePixelRatio}, '
          'kiosk ${config.kiosk}, edge guard ${config.edgeGuard}');
    }
  });
}
