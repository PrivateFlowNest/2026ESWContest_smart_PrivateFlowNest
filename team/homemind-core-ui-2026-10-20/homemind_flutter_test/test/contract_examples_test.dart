import 'dart:convert';
import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

/// Canonical examples in `<repo>/contracts/pfn.v1/examples` (read only).
Map<String, dynamic> example(String name) {
  final file = File('../../../contracts/pfn.v1/examples/$name');
  return Map<String, dynamic>.from(
      jsonDecode(file.readAsStringSync()) as Map);
}

void main() {
  test('rejected device_control shows a Korean title and reason', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage('pfn/v1/core/task/update',
        jsonEncode(example('task-update-rejected.json')));

    final task = source.current.tasks.single;
    expect(task.state, HomeTaskState.rejected);
    expect(task.title, '야간 이동 조명 켜기');
    expect(task.originKind, TaskOrigin.voice);
    expect(task.failure?.code, 'DEVICE_NOT_WIRED');
    expect(task.failure?.retryAllowed, isFalse);
    await source.dispose();
  });

  test('llm device_control proposal becomes an AI approval question',
      () async {
    final request = example('task-request-device-control.json');
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/core/task/update',
      jsonEncode({
        'schema_version': 'pfn.v1',
        'trace_id': request['trace_id'],
        'task': {
          ...request,
          'state': 'proposed',
          'revision': 1,
          'action_token': '0f8e7d6c5b4a39281706f5e4d3c2b1a0',
          'resume_allowed': false,
        },
      }),
    );

    final task = source.current.tasks.single;
    expect(task.isAiProposal, isTrue);
    expect(task.title, '거실 조명 켜기');
    expect(task.controlDeviceId, 'LED-LIVING-01');
    expect(task.sourceNodeId, 'NODE-RPI5-01');
    await source.dispose();
  });

  test('llm clarify result is kept as a home screen message', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage('pfn/v1/ai/llm/result',
        jsonEncode(example('llm-result-clarify.json')));

    final result = source.current.llmResult;
    expect(result?.status, 'clarify');
    expect(result?.isQuestion, isTrue);
    expect(result?.message, '어느 쪽을 열까요? 현관문인가요, 창문인가요?');

    source.debugHandleMessage('pfn/v1/ai/llm/result',
        jsonEncode(example('llm-result-task.json')));
    expect(source.current.llmResult?.status, 'task_requested');
    expect(source.current.llmResult?.message, isNull);
    await source.dispose();
  });

  test('perception ACTIVITY_CONTEXT turns on the quiet indicator', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage('pfn/v1/ai/perception',
        jsonEncode(example('perception-activity-context.json')));

    final activity = source.current.activity;
    expect(activity?.activityLabel, '회의 중');
    expect(activity?.audioLabel, '화상회의(스피커)');
    expect(source.current.quietMode.active, isTrue);
    expect(source.current.quietMode.byPerception, isTrue);

    // FIGURE_PRESENCE does not replace the activity context.
    source.debugHandleMessage('pfn/v1/ai/perception',
        jsonEncode(example('perception-figure-presence.json')));
    expect(source.current.activity?.activity, 'MEETING');
    await source.dispose();
  });

  test('snapshot world_state ACTIVITY_CONTEXT and quiet task are shown',
      () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/core/state/response/homemind-ui-laptop-01',
      jsonEncode({
        'tasks': [
          {
            'task_id': '5f0e0000-0000-4000-8000-000000000060',
            'trace_id': '5f0e0000-0000-4000-8000-000000000004',
            'source_node_id': 'NODE-RPI4-01',
            'intent': 'quiet_mode_on',
            'target_device': 'HOME-ENV-01',
            'priority': 40,
            'state': 'completed',
            'confidence': 0.84,
            'parameters': {'origin': 'context_rule', 'rule': 'quiet_required_on'},
            'revision': 4,
            'action_token': '0f8e7d6c5b4a39281706f5e4d3c2b1a0',
            'updated_at_ms': 1792560000400,
          }
        ],
        'devices': const <dynamic>[],
        'world_state': [
          {
            'state_key': 'desk.ACTIVITY_CONTEXT',
            'value': {
              'activity': 'WORKING',
              'audio_context': 'QUIET',
              'quiet_required': false,
              'transition': 'UNCHANGED',
            },
            'confidence': 0.7,
            'observed_at_ms': 1792560000000,
            'updated_at_ms': 1792560000100,
          }
        ],
      }),
    );

    expect(source.current.zone, HomeZone.desk);
    expect(source.current.activity?.activityLabel, '업무 중');
    expect(source.current.tasks.single.title, '조용 모드 켜기');
    expect(source.current.tasks.single.originKind, TaskOrigin.context);
    expect(source.current.quietMode.active, isTrue);
    expect(source.current.quietMode.byTask, isTrue);
    expect(
        source.current.devices
            .firstWhere((device) => device.id == 'LED-AIRCON-01')
            .displayValue,
        '깜빡임');
    await source.dispose();
  });

  test('LED light_pattern telemetry shows the blink overlay', () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage(
      'pfn/v1/device/CTRL-ESP8266-LED-01/telemetry',
      jsonEncode({
        'sent_at_ms': 1000,
        'readings': {'light_on': true, 'light_pattern': 'blink'},
        'reading_sources': {
          'light_on': 'LED-AIRCON-01',
          'light_pattern': 'LED-AIRCON-01',
        },
      }),
    );

    expect(
        source.current.devices
            .firstWhere((device) => device.id == 'LED-AIRCON-01')
            .displayValue,
        '깜빡임');
    await source.dispose();
  });
}
