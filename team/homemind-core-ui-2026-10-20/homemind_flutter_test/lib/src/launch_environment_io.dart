import 'dart:io';

import 'ui_env_file.dart';

/// `HOMEMIND_*` settings of the native process. Started by the Atlas app
/// manager (APP_ID set), they come from `$HOME/ui.env` first; variables of
/// the process still win (systemd launcher, docker -e).
Map<String, String> launchEnvironment() {
  final env = Platform.environment;
  final values = <String, String>{};
  final home = env['HOME'] ?? '';
  if ((env['APP_ID'] ?? '').isNotEmpty && home.isNotEmpty) {
    try {
      values.addAll(parseUiEnv(File('$home/$uiEnvFileName').readAsStringSync()));
    } on FileSystemException {
      // No settings yet: defaults, and the screen shows the broker as unreachable.
    } on FormatException {
      // Not UTF-8: same as no settings.
    }
  }
  for (final entry in env.entries) {
    if (entry.key.startsWith('HOMEMIND_')) values[entry.key] = entry.value;
  }
  // As an Atlas app the window is full screen, where a tap in the bottom 5 %
  // closes the app (edge swipe "BottomToTop", real Pi5-01, 2026-10-23).
  if ((env['APP_ID'] ?? '').isNotEmpty) {
    values.putIfAbsent('HOMEMIND_EDGE_GUARD', () => 'true');
  }
  return values;
}
