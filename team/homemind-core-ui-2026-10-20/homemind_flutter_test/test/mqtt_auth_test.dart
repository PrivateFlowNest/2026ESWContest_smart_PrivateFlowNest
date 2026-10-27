import 'dart:convert';
import 'dart:io';
import 'dart:math';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/mqtt_auth.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

String _randomPassword() => base64Url
    .encode(List<int>.generate(18, (_) => Random.secure().nextInt(256)));

void main() {
  late Directory dir;
  setUp(() => dir = Directory.systemTemp.createTempSync('hm-mqtt-auth-'));
  tearDown(() => dir.deleteSync(recursive: true));

  File passwordFile(String content) =>
      File('${dir.path}${Platform.pathSeparator}mqtt-password')
        ..writeAsStringSync(content);

  test('first line of the password file, trimmed like the C++ nodes', () {
    expect(passwordFromFileText('s3cret\n'), 's3cret');
    expect(passwordFromFileText(' \ts3cret \t\r\nsecond line'), 's3cret');
    expect(passwordFromFileText('s3cret'), 's3cret');
    expect(passwordFromFileText('in side spaces\n'), 'in side spaces');
    expect(passwordFromFileText('\ns3cret'), '');
    expect(passwordFromFileText(' \t \n'), '');
  });

  test('user name + password file give a login read on demand', () {
    final secret = _randomPassword();
    final file = passwordFile('$secret\n');
    final auth = MqttAuthConfig.from(environment: {
      'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01',
      'HOMEMIND_MQTT_PASSWORD_FILE': file.path,
    });
    expect(auth.error, isNull);
    expect(auth.anonymous, isFalse);
    final login = auth.login()!;
    expect(login.username, 'NODE-RPI5-01');
    expect(login.password, secret);
    expect('$login', isNot(contains(secret)));

    // A replaced file is read again on the next connect.
    final next = _randomPassword();
    file.writeAsStringSync('$next\r\n');
    expect(auth.login()!.password, next);
  });

  test('environment wins over dart-defines; nothing set is anonymous', () {
    expect(MqttAuthConfig.from().anonymous, isTrue);
    expect(MqttAuthConfig.from().login(), isNull);
    final auth = MqttAuthConfig.from(
      environment: {'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01'},
      username: 'from-define',
      passwordFile: '/data/share/homemind/secrets/mqtt-password',
    );
    expect(auth.username, 'NODE-RPI5-01');
    expect(auth.passwordFile, '/data/share/homemind/secrets/mqtt-password');
    expect(auth.error, isNull);
  });

  test('a password value is refused and never echoed', () {
    final secret = _randomPassword();
    final fromEnv = MqttAuthConfig.from(environment: {
      'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01',
      'HOMEMIND_MQTT_PASSWORD': secret,
      'HOMEMIND_MQTT_PASSWORD_FILE': passwordFile('$secret\n').path,
    });
    expect(fromEnv.error, MqttAuthConfig.refusedPasswordValue);
    expect(fromEnv.error, isNot(contains(secret)));
    expect(
        () => fromEnv.login(),
        throwsA(isA<MqttAuthException>()
            .having((e) => e.permanent, 'permanent', isTrue)
            .having((e) => e.message, 'message', isNot(contains(secret)))));

    final fromDefine = MqttAuthConfig.from(
        username: 'NODE-RPI5-01', passwordValueGiven: true);
    expect(fromDefine.error, MqttAuthConfig.refusedPasswordValue);
  });

  test('half or malformed settings are configuration errors', () {
    final noFile =
        MqttAuthConfig.from(environment: {'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01'});
    expect(noFile.error, contains('HOMEMIND_MQTT_PASSWORD_FILE'));
    final noUser = MqttAuthConfig.from(
        environment: {'HOMEMIND_MQTT_PASSWORD_FILE': '/tmp/x'});
    expect(noUser.error, contains('HOMEMIND_MQTT_USERNAME'));
    final badUser = MqttAuthConfig.from(environment: {
      'HOMEMIND_MQTT_USERNAME': 'NODE RPI5/#',
      'HOMEMIND_MQTT_PASSWORD_FILE': '/tmp/x',
    });
    expect(badUser.error, isNotNull);
  });

  test('missing or empty password files wait and retry, without the value',
      () {
    final missing = MqttAuthConfig.from(environment: {
      'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01',
      'HOMEMIND_MQTT_PASSWORD_FILE': '${dir.path}/does-not-exist',
    });
    expect(
        () => missing.login(),
        throwsA(isA<MqttAuthException>()
            .having((e) => e.permanent, 'permanent', isFalse)
            .having((e) => e.message, 'message', contains('읽을 수 없어요'))));

    for (final content in ['', ' \t\n', '\nsecond-line-only']) {
      final empty = MqttAuthConfig.from(environment: {
        'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01',
        'HOMEMIND_MQTT_PASSWORD_FILE': passwordFile(content).path,
      });
      expect(
          () => empty.login(),
          throwsA(isA<MqttAuthException>()
              .having((e) => e.message, 'message', contains('비어 있어요'))
              .having((e) => e.message, 'message',
                  isNot(contains('second-line-only')))));
    }
  });

  test('web builds stay anonymous', () {
    final web = MqttAuthConfig.from(web: true, environment: {
      'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01',
      'HOMEMIND_MQTT_PASSWORD': 'x',
    });
    expect(web.anonymous, isTrue);
  });

  test('the UI does not connect with a refused configuration', () async {
    final source = MqttHomeMindDataSource(
      host: '192.0.2.1', // TEST-NET-1: must never be contacted here
      auth: MqttAuthConfig.from(environment: {'HOMEMIND_MQTT_PASSWORD': 'x'}),
    );
    await source.start();
    expect(source.current.connected, isFalse);
    expect(source.current.connectionDetail, 'MQTT 인증 설정 오류');
    expect(source.current.contextText, MqttAuthConfig.refusedPasswordValue);
    await source.dispose();
  });
}
