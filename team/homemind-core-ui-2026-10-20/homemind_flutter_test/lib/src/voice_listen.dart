import 'models.dart';

/// Push-to-talk from the 7-inch screen (NODE-RPI5-01) to the perception node
/// (NODE-RPI5-02). Contract: `contracts/pfn.v1/voice-listen.schema.json`,
/// policy `ai_voice_listen` in `protocol.json` (QoS 1, not retained, core
/// never subscribes). Carries no audio and no text.
const kVoiceListenTopic = 'pfn/v1/ai/voice/listen';

/// RPI5-02 ignores a press older than this, so a reconnect never replays it.
const kVoiceListenLifetime = Duration(seconds: 3);

/// How long the button shows "듣는 중…" when no result arrives.
const kVoiceListenFeedback = Duration(seconds: 6);

Map<String, dynamic> buildVoiceListenPayload({
  required VoiceListenAction action,
  required String clientId,
  required String eventId,
  required String traceId,
  required int sentAtMs,
}) =>
    {
      'schema_version': 'pfn.v1',
      'event_id': eventId,
      'trace_id': traceId,
      'source_node_id': 'NODE-RPI5-01',
      'target_node_id': 'NODE-RPI5-02',
      'client_id': clientId,
      'action': action.wireValue,
      'sent_at_ms': sentAtMs,
      'expires_at_ms': sentAtMs + kVoiceListenLifetime.inMilliseconds,
    };

/// Remembers what was on screen when listening started, so the first answer
/// to the utterance (a new voice or AI task, or a new `ai/llm/result`) ends
/// the "듣는 중…" state. Periodic snapshots with the same tasks do not.
class VoiceListenWatch {
  VoiceListenWatch(HomeSnapshot snapshot)
      : _taskIds = snapshot.tasks.map((task) => task.id).toSet(),
        _llmEventId = snapshot.llmResult?.eventId;

  final Set<String> _taskIds;
  final String? _llmEventId;

  bool answeredBy(HomeSnapshot snapshot) {
    final llm = snapshot.llmResult;
    if (llm != null && llm.eventId != _llmEventId) return true;
    return snapshot.tasks.any((task) =>
        !_taskIds.contains(task.id) &&
        (task.originKind == TaskOrigin.voice ||
            task.originKind == TaskOrigin.ai));
  }
}
