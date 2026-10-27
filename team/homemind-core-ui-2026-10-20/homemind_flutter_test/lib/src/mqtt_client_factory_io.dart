import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

MqttClient createHomeMindMqttClient({
  required String host,
  required int tcpPort,
  required int webSocketPort,
  String webSocketPath = '',
  required String clientId,
}) =>
    MqttServerClient.withPort(host, clientId, tcpPort);
