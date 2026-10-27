import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/mqtt_auth.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/voice_listen.dart';
import 'package:mqtt_client/mqtt_client.dart';
import 'package:mqtt_client/mqtt_server_client.dart';

import 'support/contract_files.dart';

/// Opt-in. Run through tool/mqtt_auth_live.sh, which starts a temporary
/// Mosquitto (allow_anonymous false, package/mosquitto.acl) and homemind-core
/// as NODE-RPI4-01 in Docker and writes one random password file per account
/// into HOMEMIND_AUTH_SECRETS_DIR (removed afterwards; values never printed).
void main() {
  const runLive = bool.fromEnvironment('RUN_AUTH_BROKER_LIVE');
  const host = String.fromEnvironment('HOMEMIND_AUTH_BROKER_HOST',
      defaultValue: '127.0.0.1');
  const port =
      int.fromEnvironment('HOMEMIND_AUTH_BROKER_PORT', defaultValue: 31884);
  const secrets = String.fromEnvironment('HOMEMIND_AUTH_SECRETS_DIR');
  const uiClientId = 'homemind-ui-7inch';

  String passwordFileOf(String user) => '$secrets${Platform.pathSeparator}$user';
  String passwordOf(String user) =>
      passwordFromFileText(File(passwordFileOf(user)).readAsStringSync());

  var serial = 0;
  Future<MqttServerClient> client(String? user, {String? password}) async {
    final id = 'hm-auth-check-${++serial}-${DateTime.now().millisecond}';
    final client = MqttServerClient.withPort(host, id, port)
      ..logging(on: false)
      ..keepAlivePeriod = 10
      ..setProtocolV311()
      ..connectionMessage = MqttConnectMessage()
          .withClientIdentifier(id)
          .authenticateAs(user, user == null ? null : password ?? passwordOf(user))
          .startClean();
    try {
      await client.connect();
    } catch (_) {
      // The caller inspects connectionStatus.
    }
    return client;
  }

  Stream<({String topic, Map<String, dynamic> body})> messages(
          MqttServerClient client) =>
      client.updates!.expand((batch) => batch).map((message) {
        final publish = message.payload as MqttPublishMessage;
        final text =
            MqttPublishPayload.bytesToStringAsString(publish.payload.message);
        return (
          topic: message.topic,
          body: Map<String, dynamic>.from(jsonDecode(text) as Map)
        );
      });

  MqttHomeMindDataSource ui({String? passwordFile}) => MqttHomeMindDataSource(
        host: host,
        tcpPort: port,
        clientId: uiClientId,
        auth: MqttAuthConfig.from(environment: {
          'HOMEMIND_MQTT_USERNAME': 'NODE-RPI5-01',
          'HOMEMIND_MQTT_PASSWORD_FILE':
              passwordFile ?? passwordFileOf('NODE-RPI5-01'),
        }),
      );

  group('authenticated broker (package/mosquitto.acl)', () {
    test('anonymous and wrong passwords are refused', () async {
      final anonymous = await client(null);
      expect(anonymous.connectionStatus?.state,
          isNot(MqttConnectionState.connected));
      expect(anonymous.connectionStatus?.returnCode,
          MqttConnectReturnCode.notAuthorized);
      anonymous.disconnect();

      final wrong = await client('NODE-RPI5-01', password: 'not-the-password');
      expect(wrong.connectionStatus?.state,
          isNot(MqttConnectionState.connected));
      wrong.disconnect();

      // The UI with a stale password file says so instead of "offline".
      final stale = File(
          '${Directory.systemTemp.createTempSync('hm-stale-').path}/pw')
        ..writeAsStringSync('not-the-password\n');
      final source = ui(passwordFile: stale.path);
      try {
        await source.start();
        expect(source.current.connected, isFalse);
        expect(source.current.connectionDetail, 'MQTT 인증 거부');
        expect(source.current.contextText, startsWith('Pi4 MQTT 인증 거부'));
      } finally {
        await source.dispose();
        stale.parent.deleteSync(recursive: true);
      }
    });

    test('UI account gets state from core and sends push-to-talk', () async {
      // RPI5-02 reads voice/listen; the phone account may read state responses.
      final perception = await client('NODE-RPI5-02');
      final observer = await client('homemind-mobile-api');
      expect(perception.connectionStatus?.state, MqttConnectionState.connected);
      expect(observer.connectionStatus?.state, MqttConnectionState.connected);
      final voice = messages(perception)
          .firstWhere((m) => m.topic == kVoiceListenTopic)
          .timeout(const Duration(seconds: 10));
      final stateResponse = messages(observer)
          .firstWhere(
              (m) => m.topic == 'pfn/v1/core/state/response/$uiClientId')
          .timeout(const Duration(seconds: 15));
      perception.subscribe(kVoiceListenTopic, MqttQos.atLeastOnce);
      observer.subscribe(
          'pfn/v1/core/state/response/$uiClientId', MqttQos.atLeastOnce);
      await Future<void>.delayed(const Duration(milliseconds: 300));

      final source = ui();
      try {
        await source.start();
        expect(source.current.connected, isTrue);
        final response = await stateResponse; // homemind-core answered the UI
        expect(response.body['request_id'], isNotNull);
        await source.snapshots
            .firstWhere((s) => s.contextText.startsWith('Pi4 연결됨'))
            .timeout(const Duration(seconds: 15));

        expect(await source.requestVoiceListen(VoiceListenAction.start), isTrue);
        final press = await voice;
        expect(schemaErrors(contractJson('voice-listen.schema.json'), press.body),
            isEmpty);
        expect(press.body['client_id'], uiClientId);
      } finally {
        await source.dispose();
        perception.disconnect();
        observer.disconnect();
      }
    });

    test('the ACL is enforced for the UI account', () async {
      // Core may read pfn/v1/system/#. The UI account may write heartbeat
      // but not safety/alert (only RPI5-02 raises P0), so only one arrives.
      final core = await client('NODE-RPI4-01');
      final probe = 'acl-probe-${DateTime.now().microsecondsSinceEpoch}';
      final seen = <String>[];
      final listening = messages(core).listen((m) {
        if (m.body['acl_probe'] == probe) seen.add(m.topic);
      });
      core.subscribe('pfn/v1/system/safety/alert', MqttQos.atLeastOnce);
      core.subscribe('pfn/v1/system/heartbeat', MqttQos.atLeastOnce);
      final uiAccount = await client('NODE-RPI5-01');
      await Future<void>.delayed(const Duration(milliseconds: 300));
      for (final topic in [
        'pfn/v1/system/safety/alert',
        'pfn/v1/system/heartbeat'
      ]) {
        uiAccount.publishMessage(
            topic,
            MqttQos.atLeastOnce,
            (MqttClientPayloadBuilder()
                  ..addString(jsonEncode({'acl_probe': probe})))
                .payload!);
      }
      await Future<void>.delayed(const Duration(seconds: 2));
      expect(seen, ['pfn/v1/system/heartbeat']);
      await listening.cancel();
      uiAccount.disconnect();
      core.disconnect();
    });
  }, skip: !runLive || secrets.isEmpty);
}
