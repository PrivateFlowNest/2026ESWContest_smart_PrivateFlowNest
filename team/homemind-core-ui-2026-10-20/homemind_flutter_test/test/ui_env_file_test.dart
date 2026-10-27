import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/launch_config.dart';
import 'package:homemind_flutter_test/src/mqtt_auth.dart';
import 'package:homemind_flutter_test/src/ui_env_file.dart';

void main() {
  test('Atlas app settings file: same format as the launcher ui.env', () {
    final values = parseUiEnv('# comment\r\n'
        '\n'
        'HOMEMIND_PI4_HOST=10.10.10.2\r\n'
        'HOMEMIND_MQTT_TCP_PORT=1884\n'
        'HOMEMIND_MQTT_USERNAME="NODE-RPI5-01"\n'
        "HOMEMIND_MQTT_PASSWORD_FILE='/data/profiles/0/appdata/x/home/mqtt-password'\n"
        'HOMEMIND_CLIENT_ID=homemind-ui-7inch\n'
        'HOMEMIND_KIOSK=true\n'
        'UNKNOWN_KEY=1\n'
        'no equals sign\n'
        '=no key\n');
    expect(values, {
      'HOMEMIND_PI4_HOST': '10.10.10.2',
      'HOMEMIND_MQTT_TCP_PORT': '1884',
      'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01',
      'HOMEMIND_MQTT_PASSWORD_FILE': '/data/profiles/0/appdata/x/home/mqtt-password',
      'HOMEMIND_CLIENT_ID': 'homemind-ui-7inch',
      'HOMEMIND_KIOSK': 'true',
    });

    final config = HomeMindLaunchConfig.from(environment: values);
    expect(config.host, '10.10.10.2');
    expect(config.tcpPort, 1884);
    expect(config.kiosk, isTrue);
    expect(config.edgeGuard, isFalse);
    expect(
        HomeMindLaunchConfig.from(
                environment: parseUiEnv('HOMEMIND_EDGE_GUARD=true\n'))
            .edgeGuard,
        isTrue);
    final auth = MqttAuthConfig.from(environment: values);
    expect(auth.error, isNull);
    expect(auth.username, 'NODE-RPI5-01');
  });

  test('a password value in the settings file is refused and not kept', () {
    for (final key in ['HOMEMIND_MQTT_PASSWORD', 'MQTT_PASSWORD', 'HOMEMIND_PASSWORD_X']) {
      final values = parseUiEnv('HOMEMIND_MQTT_USERNAME=NODE-RPI5-01\n$key=do-not-keep\n');
      expect(values['HOMEMIND_MQTT_PASSWORD'], '');
      expect(values.values, isNot(contains('do-not-keep')));
      expect(MqttAuthConfig.from(environment: values).error,
          MqttAuthConfig.refusedPasswordValue);
    }
  });
}
