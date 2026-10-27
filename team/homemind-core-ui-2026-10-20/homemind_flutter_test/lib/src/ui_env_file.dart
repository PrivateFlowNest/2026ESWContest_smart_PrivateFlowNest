/// Settings file of the 7-inch UI when Atlas starts it as an app.
///
/// The Atlas app manager starts `com.atlas.app.homemind_ui` with its own
/// environment (APP_ID, HOME = the app's data folder), so `HOMEMIND_*`
/// variables cannot be passed. The installer writes them to
/// `$HOME/ui.env` instead, in the format of the systemd launcher's ui.env:
/// KEY=VALUE per line, `#` comments, optional surrounding quotes.
const uiEnvFileName = 'ui.env';

const uiEnvKeys = {
  'HOMEMIND_PI4_HOST',
  'HOMEMIND_MQTT_TCP_PORT',
  'HOMEMIND_MQTT_USERNAME',
  'HOMEMIND_MQTT_PASSWORD_FILE',
  'HOMEMIND_CLIENT_ID',
  'HOMEMIND_KIOSK',
  'HOMEMIND_EDGE_GUARD',
  'HOMEMIND_DATA_MODE',
  'HOMEMIND_CAMERA_PREVIEW_URL',
};

/// Known keys of [text]. Unknown keys are skipped. Any other key containing
/// PASSWORD becomes an empty `HOMEMIND_MQTT_PASSWORD` entry, which
/// MqttAuthConfig refuses on screen; its value is never kept.
Map<String, String> parseUiEnv(String text) {
  final values = <String, String>{};
  for (var line in text.split('\n')) {
    if (line.endsWith('\r')) line = line.substring(0, line.length - 1);
    if (line.isEmpty || line.startsWith('#')) continue;
    final eq = line.indexOf('=');
    if (eq <= 0) continue;
    final key = line.substring(0, eq);
    var value = line.substring(eq + 1);
    if (value.length >= 2 &&
        ((value.startsWith('"') && value.endsWith('"')) ||
            (value.startsWith("'") && value.endsWith("'")))) {
      value = value.substring(1, value.length - 1);
    }
    if (uiEnvKeys.contains(key)) {
      values[key] = value;
    } else if (key.contains('PASSWORD')) {
      values['HOMEMIND_MQTT_PASSWORD'] = '';
    }
  }
  return values;
}
