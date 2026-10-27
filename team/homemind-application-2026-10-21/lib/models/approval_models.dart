import 'dart:convert';

/// A task core holds for approval (GET /api/v1/approvals, Mobile API 0.5).
/// The phone may approve or reject it, except opening the entrance door, which
/// is approved only on the 7-inch screen in the house ([phoneCanApprove] false).
class ApprovalItem {
  const ApprovalItem({
    required this.taskId,
    required this.title,
    required this.intent,
    required this.targetDevice,
    required this.origin,
    required this.phoneCanApprove,
    required this.waitingSinceMs,
    this.note,
  });

  factory ApprovalItem.fromJson(Map<String, dynamic> json) => ApprovalItem(
    taskId: json['task_id'] as String? ?? '',
    title: json['title'] as String? ?? '승인 요청',
    intent: json['intent'] as String? ?? '',
    targetDevice: json['target_device'] as String? ?? '',
    origin: json['origin'] as String? ?? '',
    phoneCanApprove: json['phone_can_approve'] as bool? ?? false,
    waitingSinceMs: (json['waiting_since_ms'] as num?)?.toInt() ?? 0,
    note: json['note'] as String?,
  );

  final String taskId;
  final String title;
  final String intent;
  final String targetDevice;
  final String origin;
  final bool phoneCanApprove;
  final int waitingSinceMs;
  final String? note;

  String get originLabel => switch (origin) {
    'llm' => 'AI 제안',
    'voice_parser' => '음성 명령',
    'ui' => '화면 요청',
    'context_rule' => '상황 인식',
    _ => '요청',
  };
}

/// Shown in demo mode (no server): one AI proposal and the entrance door.
class ApprovalDemoData {
  static const List<ApprovalItem> items = [
    ApprovalItem(
      taskId: 'demo-llm-aircon',
      title: '에어컨 표시등 켜기',
      intent: 'device_control',
      targetDevice: 'LED-AIRCON-01',
      origin: 'llm',
      phoneCanApprove: true,
      waitingSinceMs: 0,
    ),
    ApprovalItem(
      taskId: 'demo-door-open',
      title: '현관문 열기',
      intent: 'device_control',
      targetDevice: 'SERVO-DOOR-01',
      origin: 'voice_parser',
      phoneCanApprove: false,
      waitingSinceMs: 0,
      note: '현관문 열기는 집 안 7인치 화면에서 승인해요',
    ),
  ];
}

/// One event of GET /api/v1/events: "task" (a task changed state) or
/// "approvals" (the waiting list changed). "open" is added by the client when
/// the stream starts.
class LiveEvent {
  const LiveEvent(this.type, this.data);

  final String type;
  final Map<String, dynamic> data;

  int? get seq => (data['seq'] as num?)?.toInt();
}

/// Server-sent events from text lines: "event:" and "data:" fields, a blank
/// line ends an event, ":" lines are keep-alive comments. Data must be JSON.
Stream<LiveEvent> parseServerSentEvents(Stream<String> lines) async* {
  String? type;
  final data = StringBuffer();
  await for (final line in lines) {
    if (line.isEmpty) {
      if (data.isNotEmpty) {
        try {
          final decoded = jsonDecode(data.toString());
          if (decoded is Map) {
            yield LiveEvent(type ?? 'message', decoded.cast<String, dynamic>());
          }
        } on FormatException {
          // not ours: skip
        }
      }
      type = null;
      data.clear();
      continue;
    }
    if (line.startsWith(':')) continue;
    final colon = line.indexOf(':');
    final field = colon < 0 ? line : line.substring(0, colon);
    var value = colon < 0 ? '' : line.substring(colon + 1);
    if (value.startsWith(' ')) value = value.substring(1);
    if (field == 'event') {
      type = value;
    } else if (field == 'data') {
      if (data.isNotEmpty) data.write('\n');
      data.write(value);
    }
  }
}
