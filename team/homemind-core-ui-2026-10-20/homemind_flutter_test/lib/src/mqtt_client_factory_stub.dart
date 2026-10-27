import 'package:mqtt_client/mqtt_client.dart';

MqttClient createHomeMindMqttClient({
  required String host,
  required int tcpPort,
  required int webSocketPort,
  String webSocketPath = '',
  required String clientId,
}) =>
    throw UnsupportedError('MQTT is not supported on this platform.');
