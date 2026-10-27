import 'package:mqtt_client/mqtt_client.dart';

import 'mqtt_client_factory_stub.dart'
    if (dart.library.io) 'mqtt_client_factory_io.dart'
    if (dart.library.html) 'mqtt_client_factory_web.dart' as platform;

MqttClient createHomeMindMqttClient({
  required String host,
  required int tcpPort,
  required int webSocketPort,
  String webSocketPath = '',
  required String clientId,
}) =>
    platform.createHomeMindMqttClient(
      host: host,
      tcpPort: tcpPort,
      webSocketPort: webSocketPort,
      webSocketPath: webSocketPath,
      clientId: clientId,
    );
