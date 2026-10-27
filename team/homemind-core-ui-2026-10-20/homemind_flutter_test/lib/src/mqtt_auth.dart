import 'mqtt_password_file_stub.dart'
    if (dart.library.io) 'mqtt_password_file_io.dart' as platform;

/// MQTT login of a native build (Pi5 eLinux bundle, Windows):
/// the user name from `HOMEMIND_MQTT_USERNAME` (environment or dart-define),
/// the password only from the first line of the file named by
/// `HOMEMIND_MQTT_PASSWORD_FILE` (trimmed, like hm::BrokerConfig::password).
/// A password value (`HOMEMIND_MQTT_PASSWORD`) is refused. Web builds stay
/// anonymous (isolated PC/Pi cable listeners only).
///
/// Messages are shown on screen, so they never contain the password.
class MqttAuthConfig {
  const MqttAuthConfig({this.username = '', this.passwordFile = '', this.error});

  final String username;
  final String passwordFile;

  /// Set when the configuration itself is wrong; the UI then does not connect.
  final String? error;

  bool get anonymous => username.isEmpty && passwordFile.isEmpty && error == null;

  static final _usernamePattern = RegExp(r'^[A-Za-z0-9_.-]{1,64}$');

  static const refusedPasswordValue =
      'Pi4 MQTT 인증 설정 오류 · HOMEMIND_MQTT_PASSWORD 값은 받지 않아요. '
      '비밀번호는 HOMEMIND_MQTT_PASSWORD_FILE 파일로만 넣어 주세요';

  /// [environment] (native process) wins over the dart-define values.
  /// [passwordValueGiven]: a `HOMEMIND_MQTT_PASSWORD` dart-define exists.
  factory MqttAuthConfig.from({
    Map<String, String> environment = const {},
    String username = '',
    String passwordFile = '',
    bool passwordValueGiven = false,
    bool web = false,
  }) {
    if (web) return const MqttAuthConfig();
    if (passwordValueGiven ||
        environment.containsKey('HOMEMIND_MQTT_PASSWORD')) {
      return const MqttAuthConfig(error: refusedPasswordValue);
    }
    final user = environment['HOMEMIND_MQTT_USERNAME'] ?? username;
    final file = environment['HOMEMIND_MQTT_PASSWORD_FILE'] ?? passwordFile;
    if (user.isEmpty && file.isEmpty) return const MqttAuthConfig();
    if (!_usernamePattern.hasMatch(user)) {
      return MqttAuthConfig(
          passwordFile: file,
          error: 'Pi4 MQTT 인증 설정 오류 · 사용자 이름(HOMEMIND_MQTT_USERNAME)이 '
              '없거나 형식이 맞지 않아요');
    }
    if (file.isEmpty) {
      return MqttAuthConfig(
          username: user,
          error: 'Pi4 MQTT 인증 설정 오류 · 비밀번호 파일(HOMEMIND_MQTT_PASSWORD_FILE)이 '
              '지정되지 않았어요');
    }
    return MqttAuthConfig(username: user, passwordFile: file);
  }

  /// Reads the password file now (on every connect, so a file created or
  /// replaced later is picked up). Null for an anonymous connection.
  /// Throws [MqttAuthException] with a message that is safe to show.
  MqttLogin? login() {
    final configError = error;
    if (configError != null) throw MqttAuthException(configError, permanent: true);
    if (anonymous) return null;
    final String text;
    try {
      text = platform.readPasswordFileHead(passwordFile);
    } catch (_) {
      throw MqttAuthException(
          'Pi4 MQTT 인증 대기 · 비밀번호 파일을 읽을 수 없어요 ($passwordFile)');
    }
    final password = passwordFromFileText(text);
    if (password.isEmpty) {
      throw MqttAuthException(
          'Pi4 MQTT 인증 대기 · 비밀번호 파일이 비어 있어요 ($passwordFile)');
    }
    return MqttLogin(username, password);
  }
}

/// First line (up to CR or LF) without leading/trailing spaces and tabs.
String passwordFromFileText(String text) {
  final end = text.indexOf(RegExp(r'[\r\n]'));
  final line = end == -1 ? text : text.substring(0, end);
  return line.replaceAll(RegExp(r'^[ \t]+|[ \t]+$'), '');
}

class MqttLogin {
  const MqttLogin(this.username, this.password);
  final String username;
  final String password;

  @override
  String toString() => 'MqttLogin($username, <hidden>)';
}

class MqttAuthException implements Exception {
  const MqttAuthException(this.message, {this.permanent = false});
  final String message;

  /// A configuration error; retrying cannot help.
  final bool permanent;

  @override
  String toString() => message;
}
