import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/mqtt_auth.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

void main() {
  test('P0 test alert only on the mock and the anonymous dev broker', () {
    expect(canTestSafetyAlert(MockHomeMindDataSource()), isTrue);
    expect(canTestSafetyAlert(MqttHomeMindDataSource()), isTrue);
    expect(
      canTestSafetyAlert(MqttHomeMindDataSource(
        auth: const MqttAuthConfig(
            username: 'NODE-RPI5-01', passwordFile: '/data/share/homemind/secrets/mqtt-password'),
      )),
      isFalse,
    );
  });
}
