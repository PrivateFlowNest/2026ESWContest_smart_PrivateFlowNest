import 'dart:convert';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/launch_config.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/voice_listen.dart';

import 'support/contract_files.dart';

Map<String, dynamic> _wire(Map<String, dynamic> payload) =>
    Map<String, dynamic>.from(jsonDecode(jsonEncode(payload)) as Map);

HomeTask _task(String id, String origin, {String source = 'NODE-RPI5-02'}) =>
    HomeTask(
      id: id,
      title: id,
      detail: '',
      state: HomeTaskState.ready,
      priority: 50,
      revision: 1,
      origin: origin,
      sourceNodeId: source,
    );

void main() {
  final schema = contractJson('voice-listen.schema.json');

  test('schema checker accepts the contract example and rejects broken copies',
      () {
    final example = contractJson('examples/voice-listen-start.json');
    expect(schemaErrors(schema, example), isEmpty);

    Map<String, dynamic> broken(void Function(Map<String, dynamic>) change) {
      final copy = Map<String, dynamic>.from(example);
      change(copy);
      return copy;
    }

    final cases = <String, Map<String, dynamic>>{
      'extra field': broken((m) => m['text'] = '거실 불 켜줘'),
      'unknown action': broken((m) => m['action'] = 'stop'),
      'missing expiry': broken((m) => m.remove('expires_at_ms')),
      'bad uuid': broken((m) => m['event_id'] = 'press-1'),
      'wrong source': broken((m) => m['source_node_id'] = 'NODE-RPI5-02'),
      'wrong target': broken((m) => m['target_node_id'] = 'NODE-RPI4-01'),
      'empty client': broken((m) => m['client_id'] = ''),
      'long client': broken((m) => m['client_id'] = 'x' * 65),
      'negative time': broken((m) => m['sent_at_ms'] = -1),
      'float time': broken((m) => m['sent_at_ms'] = 1.5),
    };
    for (final entry in cases.entries) {
      expect(schemaErrors(schema, entry.value), isNotEmpty, reason: entry.key);
    }
  });

  test('push-to-talk payloads validate against voice-listen.schema.json',
      () async {
    final source = MqttHomeMindDataSource(clientId: 'homemind-ui-7inch');
    final before = DateTime.now().millisecondsSinceEpoch;
    final first = _wire(source.debugVoiceListenPayload(VoiceListenAction.start));
    final second =
        _wire(source.debugVoiceListenPayload(VoiceListenAction.start));
    final cancel =
        _wire(source.debugVoiceListenPayload(VoiceListenAction.cancel));

    for (final payload in [first, second, cancel]) {
      expect(schemaErrors(schema, payload), isEmpty, reason: '$payload');
      expect(payload['source_node_id'], 'NODE-RPI5-01');
      expect(payload['target_node_id'], 'NODE-RPI5-02');
      expect(payload['client_id'], 'homemind-ui-7inch');
      expect(payload['expires_at_ms'] - payload['sent_at_ms'], 3000);
      expect(payload['sent_at_ms'], greaterThanOrEqualTo(before));
    }
    expect(first['action'], 'start');
    expect(cancel['action'], 'cancel');
    // Fresh ids on every press; a cancel stays in the trace of its start.
    expect({first['event_id'], second['event_id'], cancel['event_id']},
        hasLength(3));
    expect(second['trace_id'], isNot(first['trace_id']));
    expect(cancel['trace_id'], second['trace_id']);
    expect(first['event_id'], isNot(first['trace_id']));
    await source.dispose();
  });

  test('topic and QoS follow protocol.json ai_voice_listen', () {
    final protocol = contractJson('protocol.json');
    expect(protocol['topics']['ai_voice_listen'], kVoiceListenTopic);
    expect(protocol['message_schemas']['ai_voice_listen'],
        'voice-listen.schema.json');
    final policy = protocol['topic_policies']['ai_voice_listen'] as Map;
    expect(policy['publishers'], ['NODE-RPI5-01']);
    expect(policy['subscribers'], ['NODE-RPI5-02']);
    expect(policy['core_subscribes'], isFalse);
    expect(policy['qos'], 1); // MqttQos.atLeastOnce in requestVoiceListen
    expect(protocol['transport_policy']['retained'], isFalse);
    expect(kVoiceListenLifetime, const Duration(seconds: 3));
  });

  test('offline sources send nothing', () async {
    final live = MqttHomeMindDataSource();
    expect(await live.requestVoiceListen(VoiceListenAction.start), isFalse);
    await live.dispose();

    final mock = MockHomeMindDataSource()..simulateVoiceReply = false;
    await mock.setConnected(false);
    expect(await mock.requestVoiceListen(VoiceListenAction.start), isFalse);
    expect(mock.voiceListenRequests, isEmpty);
    await mock.dispose();
  });

  test('listening ends on a new voice or AI task or a new llm result', () {
    final base = MockHomeMindDataSource().current.copyWith(tasks: [
      _task('old-voice', 'voice_parser'),
    ]);
    final watch = VoiceListenWatch(base);
    expect(watch.answeredBy(base), isFalse);
    // Snapshot refreshes and unrelated tasks do not count.
    expect(
        watch.answeredBy(base.copyWith(tasks: [
          _task('old-voice', 'voice_parser'),
          _task('quiet', 'context_rule', source: 'NODE-RPI4-01'),
        ])),
        isFalse);
    expect(
        watch.answeredBy(base.copyWith(tasks: [
          _task('new-voice', 'voice_parser'),
          ...base.tasks,
        ])),
        isTrue);
    expect(
        watch.answeredBy(base.copyWith(tasks: [
          _task('new-ai', 'llm', source: 'NODE-RPI5-01'),
        ])),
        isTrue);
    expect(
        watch.answeredBy(base.copyWith(
            llmResult: LlmResultInfo(
                eventId: 'clarify-1',
                status: 'clarify',
                clarificationQuestion: '어느 문을 열까요?',
                completedAt: DateTime.now()))),
        isTrue);
  });

  test('launch config: client id and native environment overrides', () {
    expect(HomeMindLaunchConfig.from().clientId, 'homemind-ui-laptop-01');
    final device = HomeMindLaunchConfig.from(
      clientId: 'homemind-ui-7inch',
      environment: {
        'HOMEMIND_PI4_HOST': 'host.docker.internal',
        'HOMEMIND_MQTT_TCP_PORT': '1884',
        'HOMEMIND_KIOSK': '1',
      },
    );
    expect(device.host, 'host.docker.internal');
    expect(device.tcpPort, 1884);
    expect(device.kiosk, isTrue);
    expect(device.clientId, 'homemind-ui-7inch');

    // Malformed values fall back instead of reaching MQTT topics or payloads.
    final rejected = HomeMindLaunchConfig.from(environment: {
      'HOMEMIND_PI4_HOST': 'user:pw@10.0.0.9',
      'HOMEMIND_MQTT_TCP_PORT': '70000',
      'HOMEMIND_CLIENT_ID': 'ui/#',
    });
    expect(rejected.host, '10.10.10.2');
    expect(rejected.tcpPort, 1883);
    expect(rejected.clientId, 'homemind-ui-laptop-01');
    expect(HomeMindLaunchConfig.from(clientId: 'x' * 65).clientId,
        'homemind-ui-laptop-01');
  });
}
