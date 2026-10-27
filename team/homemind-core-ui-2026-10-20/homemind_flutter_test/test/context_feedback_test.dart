import 'dart:async';
import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/home_mind_app.dart';
import 'package:homemind_flutter_test/src/mock_home_mind_data_source.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/mqtt_home_mind_data_source.dart';

import 'support/contract_files.dart';

/// Scenario C: "상황이 달라요" undoes a wrong quiet context (core: context-rules.json feedback_overrides).
void main() {
  test('context feedback names the perception it disputes', () async {
    final source = MqttHomeMindDataSource();
    const perceptionId = '5f0e0000-0000-4000-8000-0000000000d1';
    source.debugHandleMessage(
        'pfn/v1/ai/perception',
        jsonEncode({
          'schema_version': 'pfn.v1',
          'event_id': perceptionId,
          'observation_type': 'ACTIVITY_CONTEXT',
          'zone': 'living',
          'confidence': 0.7,
          'attributes': {
            'activity': 'MEETING',
            'audio_context': 'TV_MEDIA',
            'quiet_required': true,
            'transition': 'ACTIVATED',
          },
          'observed_at_ms': 1792560000000,
        }));
    expect(source.current.activity?.eventId, perceptionId);
    final payload = source.debugFeedbackPayload(
        type: FeedbackType.context,
        rating: FeedbackRating.negative,
        note: 'TV를 회의로 봤어요');
    expect(payload['observation_event_id'], perceptionId);
    expect(payload['feedback_type'], 'context_feedback');
    expect(payload['rating'], 'negative');
    final schema = contractJson('feedback.schema.json');
    final allowed = (schema['properties'] as Map).keys.toSet();
    expect(payload.keys.toSet().difference(allowed), isEmpty);
    expect((schema['required'] as List).every(payload.containsKey), isTrue);
    final action = source.debugFeedbackPayload(
        type: FeedbackType.action, rating: FeedbackRating.positive);
    expect(action['observation_event_id'],
        isNull); // action feedback stays separate
    await source.dispose();
  });

  testWidgets(
      'quiet banner 상황이 달라요 turns quiet mode off and keeps that meeting from turning it on',
      (tester) async {
    tester.view.physicalSize = const Size(1024, 600);
    tester.view.devicePixelRatio = 1;
    addTearDown(tester.view.reset);
    final source = MockHomeMindDataSource();
    Future<void> settle() async {
      for (var i = 0; i < 8; i++) {
        await tester.pump(const Duration(milliseconds: 300));
      }
    }

    await tester.pumpWidget(HomeMindApp(dataSource: source));
    await tester.pump();
    unawaited(source.demoMeetingStarted());
    await settle();
    expect(source.current.quietMode.active, isTrue);
    expect(find.text('상황이 달라요'), findsOneWidget);

    await tester.tap(find.text('상황이 달라요'));
    await settle();
    expect(source.current.quietMode.active, isFalse);
    expect(source.current.tasks.first.intent, 'quiet_mode_off');
    expect(find.text('상황이 달라요'), findsNothing);

    unawaited(source.demoMeetingStarted()); // the same (disputed) context again
    await settle();
    expect(source.current.quietMode.active, isFalse);

    unawaited(source.demoMeetingEnded()); // it ended: the next meeting counts again
    await settle();
    unawaited(source.demoMeetingStarted());
    await settle();
    expect(source.current.quietMode.active, isTrue);
  });
}
