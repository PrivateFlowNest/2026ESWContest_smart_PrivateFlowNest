import 'package:mqtt_client/mqtt_browser_client.dart';
import 'package:mqtt_client/mqtt_client.dart';

MqttClient createHomeMindMqttClient({
  required String host,
  required int tcpPort,
  required int webSocketPort,
  String webSocketPath = '',
  required String clientId,
}) {
  final client = MqttBrowserClient('ws://$host$webSocketPath', clientId)
    ..port = webSocketPort
    ..websocketProtocols = MqttClientConstants.protocolsSingleDefault;
  return client;
}
