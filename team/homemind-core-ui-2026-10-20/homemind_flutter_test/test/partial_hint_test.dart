import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

import 'support/contract_files.dart';

/// Scenario A: a partial-utterance hint is shown as "preparing" (never executed) until the final utterance.
void main() {
  test('the contract example hint shows as 환기 준비 and a voice task ends it',
      () async {
    final source = MqttHomeMindDataSource();
    source.debugHandleMessage('pfn/v1/ai/perception',
        jsonEncode(contractJson('examples/perception-partial-intent.json')));
    final hint = source.current.partialHint!;
    expect(hint.intent, 'VENTILATION');
    expect(hint.intentLabel, '환기');
    expect(hint.candidateDevices, ['FAN-01', 'SERVO-WINDOW-01']);

    final request = contractJson('examples/task-request-device-control.json');
    source.debugHandleMessage(
        'pfn/v1/core/task/update',
        jsonEncode({
          'schema_version': 'pfn.v1',
          'trace_id': request['trace_id'],
          'task': {
            ...request,
            'state': 'running',
            'revision': 2,
            'parameters': {
              ...(request['parameters'] as Map),
              'origin': 'voice_parser'
            },
          },
        }));
    expect(source.current.partialHint, isNull);
    await source.dispose();
  });

  test('NO_ACTION and insufficient evidence are not shown', () async {
    final source = MqttHomeMindDataSource();
    for (final change in [
      {'intent': 'NO_ACTION'},
      {'result': 'INSUFFICIENT_EVIDENCE'},
    ]) {
      final example = contractJson('examples/perception-partial-intent.json');
      (example['attributes'] as Map).addAll(change);
      source.debugHandleMessage('pfn/v1/ai/perception', jsonEncode(example));
      expect(source.current.partialHint, isNull, reason: '$change');
    }
    await source.dispose();
  });

  testWidgets('demo hint shows the preparing banner, then it goes away',
      (tester) async {
    tester.view.physicalSize = const Size(1024, 600);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    final source = MockHomeMindDataSource();
    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();
    unawaited(source.demoPartialHint());
    await tester.pump(const Duration(milliseconds: 100));
    expect(find.text('말씀을 듣는 중 · 환기 준비'), findsOneWidget);
    expect(find.text('말이 끝나면 확인해요 · 아직 실행하지 않아요'), findsOneWidget);
    for (var i = 0; i < 10; i++) {
      await tester.pump(const Duration(milliseconds: 300));
    }
    expect(find.text('말씀을 듣는 중 · 환기 준비'), findsNothing);
  });
}
