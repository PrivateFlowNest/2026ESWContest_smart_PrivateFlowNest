enum HomeZone { away, entrance, living, bed, bathroom, desk, unknown }

extension HomeZoneLabel on HomeZone {
  String get label => switch (this) {
        HomeZone.away => '외출',
        HomeZone.entrance => '현관',
        HomeZone.living => '거실',
        HomeZone.bed => '침대',
        HomeZone.bathroom => '화장실',
        HomeZone.desk => '책상',
        HomeZone.unknown => '확인 중',
      };
}

/// device-registry.json `wiring_status`.
enum WiringStatus { wired, notWired, toConfirm }

extension WiringStatusLabel on WiringStatus {
  String get label => switch (this) {
        WiringStatus.wired => '배선됨',
        WiringStatus.notWired => '미배선',
        WiringStatus.toConfirm => '미정',
      };
}

/// Who asked for a task (task.parameters.origin + task.source_node_id).
enum TaskOrigin { voice, ai, phone, context, screen, unknown }

extension TaskOriginLabel on TaskOrigin {
  String get label => switch (this) {
        TaskOrigin.voice => '음성 명령',
        TaskOrigin.ai => 'AI 제안',
        TaskOrigin.phone => '휴대폰',
        TaskOrigin.context => '상황 감지',
        TaskOrigin.screen => '화면',
        TaskOrigin.unknown => '출처 미상',
      };
}

enum HomeTaskState {
  proposed,
  ready,
  running,
  deferred,
  completed,
  failed,
  rejected,
  cancelled
}

extension HomeTaskStateLabel on HomeTaskState {
  String get label => switch (this) {
        HomeTaskState.proposed => '확인이 필요해요',
        HomeTaskState.ready => '실행 준비 중',
        HomeTaskState.running => '실행 중',
        HomeTaskState.deferred => '조용히 보류했어요',
        HomeTaskState.completed => '완료했어요',
        HomeTaskState.failed => '실행하지 못했어요',
        HomeTaskState.rejected => '실행하지 않아요',
        HomeTaskState.cancelled => '취소했어요',
      };
}

enum TaskAction { approve, reject, cancel }

/// `pfn/v1/ai/voice/listen` action (push-to-talk on the 7-inch screen).
enum VoiceListenAction { start, cancel }

extension VoiceListenActionWire on VoiceListenAction {
  String get wireValue => switch (this) {
        VoiceListenAction.start => 'start',
        VoiceListenAction.cancel => 'cancel',
      };
}

enum PrivacyMode { normal, cameraBlocked, audioBlocked, private }

extension PrivacyModeLabel on PrivacyMode {
  String get label => switch (this) {
        PrivacyMode.normal => '일반 모드',
        PrivacyMode.cameraBlocked => '카메라 차단',
        PrivacyMode.audioBlocked => '마이크 차단',
        PrivacyMode.private => '완전 개인정보 모드',
      };

  String get wireValue => switch (this) {
        PrivacyMode.normal => 'normal',
        PrivacyMode.cameraBlocked => 'camera_blocked',
        PrivacyMode.audioBlocked => 'audio_blocked',
        PrivacyMode.private => 'private',
      };
}

enum FeedbackType { context, action }

enum FeedbackRating { positive, negative, unknown }

class PrivacyState {
  const PrivacyState({
    required this.configured,
    required this.mode,
    required this.cameraAllowed,
    required this.microphoneAllowed,
    required this.recording,
    required this.localProcessing,
    this.changedAt,
  });

  const PrivacyState.initial()
      : configured = false,
        mode = PrivacyMode.normal,
        cameraAllowed = true,
        microphoneAllowed = true,
        recording = false,
        localProcessing = true,
        changedAt = null;

  final bool configured;
  final PrivacyMode mode;
  final bool cameraAllowed;
  final bool microphoneAllowed;
  final bool recording;
  final bool localProcessing;
  final DateTime? changedAt;
}

class SafetyAlertDetail {
  const SafetyAlertDetail({
    required this.alertId,
    required this.traceId,
    required this.type,
    required this.zone,
    required this.summary,
    required this.raisedAt,
    this.automaticActions = const [],
  });

  final String alertId;
  final String traceId;
  final String type;
  final String zone;
  final String summary;
  final DateTime raisedAt;
  final List<String> automaticActions;
}

class TaskFailure {
  const TaskFailure(
      {required this.code,
      required this.message,
      required this.occurredAt,
      this.retryAllowed = true});
  final String code;
  final String message;
  final DateTime occurredAt;
  final bool retryAllowed;
}

class HomeTask {
  const HomeTask(
      {required this.id,
      required this.title,
      required this.detail,
      required this.state,
      required this.priority,
      required this.revision,
      this.traceId = '',
      this.actionToken = '',
      this.intent = '',
      this.targetDevice = '',
      this.confidence = 0,
      this.updatedAt,
      this.failure,
      this.origin = '',
      this.sourceNodeId = '',
      this.parameters = const {}});
  final String id;
  final String title;
  final String detail;
  final HomeTaskState state;
  final int priority;
  final int revision;
  final String traceId;
  final String actionToken;
  final String intent;
  final String targetDevice;
  final double confidence;
  final DateTime? updatedAt;
  final TaskFailure? failure;

  /// Raw `parameters.origin` (voice_parser, llm, ui, context_rule) or ''.
  final String origin;

  /// Node that sent the task request (task.source_node_id).
  final String sourceNodeId;

  /// Raw task.parameters. For `device_control` this holds target_device_id,
  /// action, parameters, origin, ai_device and ai_action.
  final Map<String, dynamic> parameters;

  bool get isDeviceControl => intent == 'device_control';

  String get controlDeviceId =>
      parameters['target_device_id']?.toString() ?? targetDevice;

  String get controlAction => parameters['action']?.toString() ?? '';

  Map<String, dynamic> get controlParameters {
    final value = parameters['parameters'];
    return value is Map ? Map<String, dynamic>.from(value) : const {};
  }

  TaskOrigin get originKind => switch (origin) {
        'voice_parser' => TaskOrigin.voice,
        'llm' => TaskOrigin.ai,
        'context_rule' => TaskOrigin.context,
        'ui' when sourceNodeId == 'NODE-RPI4-01' => TaskOrigin.phone,
        'ui' => TaskOrigin.screen,
        _ when sourceNodeId == 'NODE-RPI5-01' => TaskOrigin.screen,
        _ => TaskOrigin.unknown,
      };

  bool get isAiProposal =>
      state == HomeTaskState.proposed && originKind == TaskOrigin.ai;

  HomeTask copyWith(
          {HomeTaskState? state,
          int? revision,
          String? traceId,
          String? actionToken,
          String? intent,
          String? targetDevice,
          double? confidence,
          DateTime? updatedAt,
          TaskFailure? failure,
          bool clearFailure = false}) =>
      HomeTask(
        id: id,
        title: title,
        detail: detail,
        state: state ?? this.state,
        priority: priority,
        revision: revision ?? this.revision,
        traceId: traceId ?? this.traceId,
        actionToken: actionToken ?? this.actionToken,
        intent: intent ?? this.intent,
        targetDevice: targetDevice ?? this.targetDevice,
        confidence: confidence ?? this.confidence,
        updatedAt: updatedAt ?? this.updatedAt,
        failure: clearFailure ? null : failure ?? this.failure,
        origin: origin,
        sourceNodeId: sourceNodeId,
        parameters: parameters,
      );
}

class DeviceStatus {
  const DeviceStatus(
      {required this.id,
      required this.name,
      required this.location,
      required this.online,
      required this.active,
      required this.displayValue,
      required this.controllerNodeId,
      this.category = '기기',
      this.lastSeenAt,
      this.offlineAfterSeconds = 15,
      this.wiringStatus = WiringStatus.wired,
      this.hardware = '',
      this.pin = ''});
  final String id;
  final String name;
  final String location;
  final bool online;
  final bool active;
  final String displayValue;
  final String controllerNodeId;
  final String category;
  final DateTime? lastSeenAt;
  final int offlineAfterSeconds;
  final WiringStatus wiringStatus;
  final String hardware;
  final String pin;

  bool get wired => wiringStatus == WiringStatus.wired;

  DeviceStatus copyWith(
          {bool? online,
          bool? active,
          String? displayValue,
          DateTime? lastSeenAt}) =>
      DeviceStatus(
        id: id,
        name: name,
        location: location,
        online: online ?? this.online,
        active: active ?? this.active,
        displayValue: displayValue ?? this.displayValue,
        controllerNodeId: controllerNodeId,
        category: category,
        lastSeenAt: lastSeenAt ?? this.lastSeenAt,
        offlineAfterSeconds: offlineAfterSeconds,
        wiringStatus: wiringStatus,
        hardware: hardware,
        pin: pin,
      );
}

class SensorReading {
  const SensorReading(
      {required this.key,
      required this.label,
      required this.value,
      required this.unit,
      required this.sourceDeviceId,
      required this.updatedAt,
      this.valid = true});
  final String key;
  final String label;
  final String value;
  final String unit;
  final String sourceDeviceId;
  final DateTime updatedAt;
  final bool valid;
}

class SystemComponentStatus {
  const SystemComponentStatus(
      {required this.id,
      required this.name,
      required this.status,
      this.detail = ''});
  final String id;
  final String name;
  final String status;
  final String detail;
}

class HomeSystemStatus {
  const HomeSystemStatus(
      {required this.overallStatus,
      required this.storageUsedPercent,
      required this.warningThresholdPercent,
      required this.storageWarning,
      required this.databaseBytes,
      required this.generatedAt,
      required this.components,
      this.timeValid = false,
      this.bootUptimeMs = 0});
  final String overallStatus;
  final double storageUsedPercent;
  final double warningThresholdPercent;
  final bool storageWarning;
  final int databaseBytes;
  final DateTime generatedAt;
  final List<SystemComponentStatus> components;
  final bool timeValid;
  final int bootUptimeMs;

  HomeSystemStatus copyWith({
    String? overallStatus,
    double? storageUsedPercent,
    double? warningThresholdPercent,
    bool? storageWarning,
    int? databaseBytes,
    DateTime? generatedAt,
    List<SystemComponentStatus>? components,
    bool? timeValid,
    int? bootUptimeMs,
  }) =>
      HomeSystemStatus(
        overallStatus: overallStatus ?? this.overallStatus,
        storageUsedPercent: storageUsedPercent ?? this.storageUsedPercent,
        warningThresholdPercent:
            warningThresholdPercent ?? this.warningThresholdPercent,
        storageWarning: storageWarning ?? this.storageWarning,
        databaseBytes: databaseBytes ?? this.databaseBytes,
        generatedAt: generatedAt ?? this.generatedAt,
        components: components ?? this.components,
        timeValid: timeValid ?? this.timeValid,
        bootUptimeMs: bootUptimeMs ?? this.bootUptimeMs,
      );
}

/// Latest `pfn/v1/ai/perception` ACTIVITY_CONTEXT, or the matching
/// `{zone}.ACTIVITY_CONTEXT` world_state row of the Pi4 snapshot.
class ActivityContext {
  const ActivityContext({
    required this.activity,
    required this.audioContext,
    required this.quietRequired,
    required this.transition,
    required this.zone,
    required this.confidence,
    required this.observedAt,
    this.eventId,
  });

  final String activity;
  final String audioContext;
  final bool quietRequired;
  final String transition;
  final String zone;
  final double confidence;
  final DateTime observedAt;

  /// `event_id` of the perception (sent back as observation_event_id with "상황이 달라요").
  final String? eventId;

  String get activityLabel => switch (activity) {
        'RESTING' => '휴식 중',
        'SLEEPING' => '수면 중',
        'STUDYING' => '공부 중',
        'WORKING' => '업무 중',
        'MEETING' => '회의 중',
        'BATHROOM_USE' => '화장실 사용 중',
        'ENTERING' => '귀가 중',
        'LEAVING' => '외출 중',
        'GENERAL_ACTIVITY' => '일상 활동',
        _ => '활동 확인 중',
      };

  String get audioLabel => switch (audioContext) {
        'QUIET' => '조용함',
        'SELF_TALK' => '혼잣말',
        'TV_MEDIA' => 'TV 시청',
        'ZOOM_SPEAKER' => '화상회의(스피커)',
        'ZOOM_HEADPHONES' => '화상회의(헤드폰)',
        'IN_PERSON_MEETING' => '대면 대화',
        _ => '소리 확인 중',
      };
}

/// Latest `pfn/v1/ai/llm/result`. The contract never carries the transcript.
class LlmResultInfo {
  const LlmResultInfo({
    required this.eventId,
    required this.status,
    required this.completedAt,
    this.taskId,
    this.aiIntent,
    this.aiDevice,
    this.aiAction,
    this.clarificationQuestion,
    this.safetyFlags = const [],
  });

  final String eventId;
  final String status;
  final DateTime completedAt;
  final String? taskId;
  final String? aiIntent;
  final String? aiDevice;
  final String? aiAction;
  final String? clarificationQuestion;
  final List<String> safetyFlags;

  bool get isQuestion => status == 'clarify';

  /// Korean message for the home screen. null when there is nothing to show
  /// (`task_requested` is shown through the proposal card instead).
  String? get message => switch (status) {
        'clarify' => (clarificationQuestion?.trim().isNotEmpty ?? false)
            ? clarificationQuestion!.trim()
            : '조금 더 자세히 말씀해 주세요.',
        'no_action' => '실행할 작업이 없다고 판단했어요.',
        'rejected_guard' => '안전 규칙에 맞지 않아 AI가 제안하지 않았어요.',
        'invalid_output' => 'AI가 요청을 이해하지 못했어요. 다시 말씀해 주세요.',
        'model_unavailable' => 'AI 모델을 지금 사용할 수 없어요. 잠시 후 다시 말씀해 주세요.',
        'expired' => '요청 시간이 지나 처리하지 않았어요.',
        _ => null,
      };
}

class QuietModeStatus {
  const QuietModeStatus({
    required this.active,
    this.byTask = false,
    this.byPerception = false,
    this.origin = TaskOrigin.unknown,
  });

  final bool active;
  final bool byTask;
  final bool byPerception;
  final TaskOrigin origin;
}

/// PARTIAL_INTENT_HINT from NODE-RPI5-02 (Vosk partial text -> intent while the person is still speaking).
/// Read-only: execution_allowed is always false; a task only follows the final utterance.
class PartialIntentHint {
  const PartialIntentHint({
    required this.intent,
    required this.candidateDevices,
    required this.confidence,
    required this.observedAt,
  });

  /// How long the screen shows "preparing" after the last hint.
  static const freshFor = Duration(seconds: 8);

  final String intent;
  final List<String> candidateDevices;
  final double confidence;
  final DateTime observedAt;

  bool freshAt(DateTime now) => now.difference(observedAt) < freshFor;

  String get intentLabel => switch (intent) {
        'CLIMATE_COMFORT' => '냉난방',
        'LIGHTING' => '조명',
        'VENTILATION' => '환기',
        'MEDIA' => 'TV·미디어',
        'ACCESS' => '문',
        'LAUNDRY' => '세탁',
        'CLEANING' => '청소',
        'QUIET_ENVIRONMENT' => '조용한 환경',
        _ => '요청',
      };
}

class HomeSnapshot {
  const HomeSnapshot(
      {required this.connected,
      required this.zone,
      required this.contextText,
      required this.tasks,
      required this.devices,
      required this.safetyAlert,
      required this.lastUpdated,
      required this.systemStatus,
      this.sensorReadings = const [],
      this.connectionDetail = '',
      this.privacy = const PrivacyState.initial(),
      this.safetyDetail,
      this.activity,
      this.llmResult,
      this.partialHint});
  final bool connected;
  final HomeZone zone;
  final String contextText;
  final List<HomeTask> tasks;
  final List<DeviceStatus> devices;
  final List<SensorReading> sensorReadings;
  final bool safetyAlert;
  final DateTime lastUpdated;
  final String connectionDetail;
  final HomeSystemStatus systemStatus;
  final PrivacyState privacy;
  final SafetyAlertDetail? safetyDetail;
  final ActivityContext? activity;
  final LlmResultInfo? llmResult;
  final PartialIntentHint? partialHint;

  /// Quiet mode is on when the newest started quiet task is `quiet_mode_on`
  /// (no later `quiet_mode_off`), or perception reports quiet_required — unless
  /// the newest quiet task is core undoing a context the user disputed
  /// ("상황이 달라요", context-rules.json feedback override `quiet_context_wrong`).
  QuietModeStatus get quietMode {
    HomeTask? latest;
    var latestIndex = 0;
    for (var index = 0; index < tasks.length; index++) {
      final task = tasks[index];
      if (task.intent != 'quiet_mode_on' && task.intent != 'quiet_mode_off') {
        continue;
      }
      if (task.state != HomeTaskState.ready &&
          task.state != HomeTaskState.running &&
          task.state != HomeTaskState.completed) {
        continue;
      }
      final current = latest;
      if (current == null) {
        latest = task;
        latestIndex = index;
        continue;
      }
      final a = task.updatedAt;
      final b = current.updatedAt;
      final newer =
          a != null && b != null && a != b ? a.isAfter(b) : index < latestIndex;
      if (newer) {
        latest = task;
        latestIndex = index;
      }
    }
    final byTask = latest?.intent == 'quiet_mode_on';
    final disputed = latest?.intent == 'quiet_mode_off' &&
        latest?.parameters['rule'] == 'quiet_context_wrong';
    final byPerception = !disputed && activity?.quietRequired == true;
    return QuietModeStatus(
      active: byTask || byPerception,
      byTask: byTask,
      byPerception: byPerception,
      origin: byTask ? latest!.originKind : TaskOrigin.context,
    );
  }

  HomeSnapshot copyWith(
          {bool? connected,
          HomeZone? zone,
          String? contextText,
          List<HomeTask>? tasks,
          List<DeviceStatus>? devices,
          List<SensorReading>? sensorReadings,
          bool? safetyAlert,
          DateTime? lastUpdated,
          String? connectionDetail,
          HomeSystemStatus? systemStatus,
          PrivacyState? privacy,
          SafetyAlertDetail? safetyDetail,
          bool clearSafetyDetail = false,
          ActivityContext? activity,
          LlmResultInfo? llmResult,
          PartialIntentHint? partialHint,
          bool clearPartialHint = false}) =>
      HomeSnapshot(
        connected: connected ?? this.connected,
        zone: zone ?? this.zone,
        contextText: contextText ?? this.contextText,
        tasks: tasks ?? this.tasks,
        devices: devices ?? this.devices,
        sensorReadings: sensorReadings ?? this.sensorReadings,
        safetyAlert: safetyAlert ?? this.safetyAlert,
        lastUpdated: lastUpdated ?? this.lastUpdated,
        connectionDetail: connectionDetail ?? this.connectionDetail,
        systemStatus: systemStatus ?? this.systemStatus,
        privacy: privacy ?? this.privacy,
        safetyDetail:
            clearSafetyDetail ? null : safetyDetail ?? this.safetyDetail,
        activity: activity ?? this.activity,
        llmResult: llmResult ?? this.llmResult,
        partialHint: clearPartialHint ? null : partialHint ?? this.partialHint,
      );
}
