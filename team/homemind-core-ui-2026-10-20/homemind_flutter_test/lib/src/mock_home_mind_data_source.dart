import 'dart:async';
import 'dart:convert';

import 'device_catalog.dart';
import 'home_mind_data_source.dart';
import 'models.dart';
import 'phone_pairing.dart';
import 'task_presentation.dart';

class MockHomeMindDataSource implements HomeMindDataSource, HomeMindDemoActions {
  /// [seedDemoData] fills the screens with one task of every core 0.6.0 kind
  /// (AI proposal, voice command, rejection, quiet mode, phone command).
  MockHomeMindDataSource({bool seedDemoData = false})
      : _current = _initialSnapshot() {
    if (seedDemoData) _seed();
  }

  static HomeSnapshot _initialSnapshot() {
    final now = DateTime.now();
    return HomeSnapshot(
      connected: true,
      zone: HomeZone.away,
      contextText: '노트북 가상 데이터 모드 · 집이 비어 있습니다',
      tasks: const [],
      devices: buildOfficialDeviceStatuses(online: true),
      sensorReadings: [
        SensorReading(
            key: 'sound_level_db',
            label: '실내 소음',
            value: '34',
            unit: 'dB',
            sourceDeviceId: 'MIC-INDOOR-01',
            updatedAt: now),
        SensorReading(
            key: 'fan_level_percent',
            label: '화장실 환기 팬 세기',
            value: '0',
            unit: '%',
            sourceDeviceId: 'FAN-01',
            updatedAt: now),
      ],
      safetyAlert: false,
      lastUpdated: now,
      connectionDetail: '가상 데이터',
      systemStatus: HomeSystemStatus(
        overallStatus: 'healthy',
        storageUsedPercent: 23,
        warningThresholdPercent: 70,
        storageWarning: false,
        databaseBytes: 3145728,
        generatedAt: now,
        timeValid: true,
        components: const [
          SystemComponentStatus(
              id: 'mqtt', name: 'MQTT 브로커', status: 'healthy', detail: '가상 정상'),
          SystemComponentStatus(
              id: 'core', name: 'Pi4 백엔드', status: 'healthy', detail: '가상 정상'),
          SystemComponentStatus(
              id: 'sqlite',
              name: 'SQLite',
              status: 'healthy',
              detail: '무결성 정상'),
        ],
      ),
    );
  }

  final StreamController<HomeSnapshot> _controller =
      StreamController<HomeSnapshot>.broadcast();
  HomeSnapshot _current;
  bool _disposed = false;
  bool _quietDisputed = false;
  int _sequence = 0;
  int _aiProposalIndex = 0;

  /// Short pause between ready → running → completed in the mock.
  Duration stepDelay = const Duration(milliseconds: 300);

  /// Every push-to-talk request, in order (the mock sends nothing).
  final List<VoiceListenAction> voiceListenRequests = [];

  /// After 말하기, pretend RPI5-02 heard "거실 불 켜줘" once [voiceReplyDelay]
  /// has passed. Tests turn this off to see the timeout path.
  bool simulateVoiceReply = true;
  Duration voiceReplyDelay = const Duration(milliseconds: 1500);
  Timer? _voiceReplyTimer;

  @override
  bool get isLive => false;
  @override
  Stream<HomeSnapshot> get snapshots => _controller.stream;
  @override
  HomeSnapshot get current => _current;
  @override
  Future<void> start() async => _emit();
  @override
  Future<void> requestSnapshot() async => _emit();

  void _emit() {
    if (_disposed) return;
    _current = _current.copyWith(lastUpdated: DateTime.now());
    _controller.add(_current);
  }

  String _nextId(String prefix) => '$prefix-${++_sequence}';

  void _upsertTask(HomeTask next) {
    final tasks = [..._current.tasks];
    final index = tasks.indexWhere((task) => task.id == next.id);
    if (index == -1) {
      tasks.insert(0, next);
    } else {
      tasks[index] = next;
    }
    _current = _current.copyWith(tasks: tasks);
  }

  HomeTask _scenarioTask(String id, String intent, HomeTaskState state,
      {String origin = 'ui',
      String sourceNodeId = 'NODE-RPI5-01',
      String? rule,
      DateTime? updatedAt}) {
    final text = describeTask(intent, const {});
    return HomeTask(
      id: id,
      title: text.title,
      detail: text.detail,
      state: state,
      priority: 40,
      revision: 1,
      intent: intent,
      targetDevice: 'HOME-ENV-01',
      confidence: 0.9,
      origin: origin,
      sourceNodeId: sourceNodeId,
      parameters: {'origin': origin, if (rule != null) 'rule': rule},
      updatedAt: updatedAt ?? DateTime.now(),
    );
  }

  HomeTask _controlTask(
    String id, {
    required String deviceId,
    required String action,
    required Map<String, dynamic> args,
    required String origin,
    required String sourceNodeId,
    required HomeTaskState state,
    String? aiDevice,
    String? aiAction,
    TaskFailure? failure,
    DateTime? updatedAt,
  }) {
    final parameters = <String, dynamic>{
      'target_device_id': deviceId,
      'action': action,
      'parameters': args,
      'origin': origin,
      'ai_device': aiDevice,
      'ai_action': aiAction,
    };
    final text = describeTask('device_control', parameters);
    return HomeTask(
      id: id,
      title: text.title,
      detail: text.detail,
      state: state,
      priority: 50,
      revision: state == HomeTaskState.proposed ? 1 : 2,
      intent: 'device_control',
      targetDevice: deviceId,
      confidence: origin == 'llm' ? 0.86 : 0.95,
      origin: origin,
      sourceNodeId: sourceNodeId,
      parameters: parameters,
      failure: failure,
      updatedAt: updatedAt ?? DateTime.now(),
    );
  }

  /// ready → running → completed, like core's auto-approved tasks.
  Future<void> _runAutomatically(HomeTask task) async {
    _upsertTask(task.copyWith(state: HomeTaskState.ready));
    _current = _current.copyWith(contextText: '${task.title} · 실행 준비 중');
    _emit();
    await Future<void>.delayed(stepDelay);
    if (_disposed) return;
    _upsertTask(task.copyWith(
        state: HomeTaskState.running,
        revision: task.revision + 1,
        updatedAt: DateTime.now()));
    _emit();
    await Future<void>.delayed(stepDelay);
    if (_disposed) return;
    final done = task.copyWith(
        state: HomeTaskState.completed,
        revision: task.revision + 2,
        updatedAt: DateTime.now());
    _upsertTask(done);
    _current = _current.copyWith(
      contextText: '${task.title} · 완료했어요',
      devices: applyTaskEffects(_current.devices, done),
    );
    _emit();
  }

  void _seed() {
    final now = DateTime.now();
    final light = _controlTask(_nextId('voice'),
        deviceId: 'LED-LIVING-01',
        action: 'set_switch',
        args: {'on': true},
        origin: 'voice_parser',
        sourceNodeId: 'NODE-RPI5-02',
        aiDevice: 'LIGHT',
        aiAction: 'TURN_ON',
        state: HomeTaskState.completed,
        updatedAt: now.subtract(const Duration(minutes: 9)));
    final window = _controlTask(_nextId('phone'),
        deviceId: 'SERVO-WINDOW-01',
        action: 'set_position',
        args: {'position_degrees': 0},
        origin: 'ui',
        sourceNodeId: 'NODE-RPI4-01',
        state: HomeTaskState.completed,
        updatedAt: now.subtract(const Duration(minutes: 12)));
    final quiet = _scenarioTask(
        _nextId('context'), 'quiet_mode_on', HomeTaskState.completed,
        origin: 'context_rule',
        sourceNodeId: 'NODE-RPI4-01',
        updatedAt: now.subtract(const Duration(minutes: 3)));
    final rejected = _controlTask(_nextId('voice'),
        deviceId: 'LED-GUIDE-01',
        action: 'set_switch',
        args: {'on': true},
        origin: 'voice_parser',
        sourceNodeId: 'NODE-RPI5-02',
        state: HomeTaskState.rejected,
        failure: TaskFailure(
            code: 'DEVICE_NOT_WIRED',
            message: 'LED-GUIDE-01 wiring_status is not_wired',
            occurredAt: now.subtract(const Duration(minutes: 2)),
            retryAllowed: false),
        updatedAt: now.subtract(const Duration(minutes: 2)));
    final proposal = _controlTask(_nextId('llm'),
        deviceId: 'LED-AIRCON-01',
        action: 'set_switch',
        args: {'on': true},
        origin: 'llm',
        sourceNodeId: 'NODE-RPI5-01',
        aiDevice: 'AIR_CONDITIONER',
        aiAction: 'TURN_ON',
        state: HomeTaskState.proposed,
        updatedAt: now.subtract(const Duration(seconds: 20)));
    var devices = _current.devices;
    for (final task in [window, light, quiet]) {
      devices = applyTaskEffects(devices, task);
    }
    _current = _current.copyWith(
      zone: HomeZone.desk,
      contextText: '책상에서 화상회의 중 · 조용 모드',
      tasks: [proposal, rejected, quiet, light, window],
      devices: devices,
      activity: ActivityContext(
          activity: 'MEETING',
          audioContext: 'ZOOM_SPEAKER',
          quietRequired: true,
          transition: 'ACTIVATED',
          zone: 'desk',
          confidence: 0.84,
          observedAt: now.subtract(const Duration(minutes: 3))),
      llmResult: LlmResultInfo(
          eventId: _nextId('llm-result'),
          status: 'task_requested',
          taskId: proposal.id,
          aiIntent: 'DEVICE_CONTROL',
          aiDevice: 'AIR_CONDITIONER',
          aiAction: 'TURN_ON',
          completedAt: now.subtract(const Duration(seconds: 20))),
    );
  }

  @override
  Future<void> simulateArrival() async {
    if (!_current.connected) return;
    final task = _scenarioTask('arrival-demo-task', 'prepare_home',
            HomeTaskState.running)
        .copyWith(revision: 1);
    _upsertTask(task);
    _current = _current.copyWith(
        zone: HomeZone.entrance, contextText: '귀가 감지 · 환경을 준비하는 중');
    _emit();
    await Future<void>.delayed(const Duration(milliseconds: 500));
    final done = task.copyWith(state: HomeTaskState.completed, revision: 2);
    _upsertTask(done);
    _current = _current.copyWith(
      zone: HomeZone.living,
      contextText: '귀가 완료 · 편안한 실내 환경',
      devices: applyTaskEffects(_current.devices, done),
    );
    _emit();
  }

  @override
  Future<void> proposeSleep() async {
    if (!_current.connected) return;
    _upsertTask(_scenarioTask(
        'sleep-demo-task', 'prepare_sleep', HomeTaskState.proposed));
    _current = _current.copyWith(
        zone: HomeZone.bed, contextText: '취침 행동 감지 · 사용자 확인 필요');
    _emit();
  }

  @override
  Future<void> actOnTask(String taskId, TaskAction action) async {
    if (!_current.connected) return;
    final index = _current.tasks.indexWhere((task) => task.id == taskId);
    if (index == -1) return;
    final task = _current.tasks[index];
    if (action == TaskAction.reject || action == TaskAction.cancel) {
      _upsertTask(task.copyWith(
          state: action == TaskAction.reject
              ? HomeTaskState.rejected
              : HomeTaskState.cancelled,
          revision: task.revision + 1,
          updatedAt: DateTime.now()));
      _current = _current.copyWith(
          contextText:
              action == TaskAction.reject ? '작업을 실행하지 않았습니다' : '작업을 취소했습니다');
      _emit();
      return;
    }
    _upsertTask(task.copyWith(
        state: HomeTaskState.running,
        revision: task.revision + 1,
        clearFailure: true,
        updatedAt: DateTime.now()));
    _current = _current.copyWith(contextText: '승인됨 · 명령 ACK 대기 중');
    _emit();
    await Future<void>.delayed(const Duration(milliseconds: 500));
    final done = task.copyWith(
        state: HomeTaskState.completed,
        revision: task.revision + 2,
        clearFailure: true,
        updatedAt: DateTime.now());
    _upsertTask(done);
    _current = _current.copyWith(
      contextText: '명령 ACK 수신 · ${task.title} 완료',
      devices: applyTaskEffects(_current.devices, done),
      sensorReadings: task.intent == 'prepare_sleep'
          ? _replaceReading('fan_level_percent', '0')
          : null,
    );
    _emit();
  }

  List<SensorReading> _replaceReading(String key, String value) =>
      _current.sensorReadings
          .map((reading) => reading.key == key
              ? SensorReading(
                  key: reading.key,
                  label: reading.label,
                  value: value,
                  unit: reading.unit,
                  sourceDeviceId: reading.sourceDeviceId,
                  updatedAt: DateTime.now())
              : reading)
          .toList(growable: false);

  @override
  Future<void> simulateNightExit() async {
    if (!_current.connected) return;
    final task = _scenarioTask(
        'night-exit-demo-task', 'night_bed_exit', HomeTaskState.running);
    _upsertTask(task);
    _current = _current.copyWith(
        zone: HomeZone.living,
        contextText: '침대 이탈 감지 · 이동 조명 켜짐',
        devices: applyTaskEffects(_current.devices, task));
    _emit();
    await Future<void>.delayed(const Duration(milliseconds: 700));
    _upsertTask(task.copyWith(state: HomeTaskState.completed, revision: 2));
    _current = _current.copyWith(
        zone: HomeZone.bed,
        contextText: '침대 복귀 완료 · 이동 조명 꺼짐',
        devices: _current.devices
            .map((device) => device.id == 'LED-LIVING-01'
                ? device.copyWith(active: false, displayValue: 'OFF')
                : device)
            .toList(growable: false));
    _emit();
  }

  @override
  Future<void> simulateCommandTimeout() async {
    final now = DateTime.now();
    final parameters = <String, dynamic>{
      'target_device_id': 'FAN-01',
      'action': 'set_switch',
      'parameters': {'on': true},
      'origin': 'ui',
      'ai_device': null,
      'ai_action': null,
    };
    final text = describeTask('device_control', parameters);
    _upsertTask(HomeTask(
      id: 'timeout-demo-${now.millisecondsSinceEpoch}',
      title: text.title,
      detail: 'FAN-01에 명령을 보냈지만 ACK를 받지 못했습니다.',
      state: HomeTaskState.failed,
      priority: 2,
      revision: 4,
      intent: 'device_control',
      targetDevice: 'FAN-01',
      origin: 'ui',
      sourceNodeId: 'NODE-RPI5-01',
      parameters: parameters,
      updatedAt: now,
      failure: TaskFailure(
          code: 'ACK_TIMEOUT',
          message: '3초씩 3번 기다렸지만 장치 응답이 없습니다.',
          occurredAt: now),
    ));
    _current = _current.copyWith(contextText: '명령 실패 · FAN-01 연결을 확인하세요');
    _emit();
  }

  @override
  Future<void> simulateDeviceOffline() async {
    final stale = DateTime.now().subtract(const Duration(seconds: 16));
    _current = _current.copyWith(
      contextText: 'CTRL-ESP32-ACT-01 신호가 15초 동안 없어 오프라인 처리됨',
      devices: _current.devices
          .map((device) => device.controllerNodeId == 'CTRL-ESP32-ACT-01'
              ? device.copyWith(online: false, lastSeenAt: stale)
              : device)
          .toList(growable: false),
      systemStatus: HomeSystemStatus(
        overallStatus: 'degraded',
        storageUsedPercent: _current.systemStatus.storageUsedPercent,
        warningThresholdPercent: 70,
        storageWarning: false,
        databaseBytes: _current.systemStatus.databaseBytes,
        generatedAt: DateTime.now(),
        timeValid: _current.systemStatus.timeValid,
        components: const [
          SystemComponentStatus(
              id: 'mqtt', name: 'MQTT 브로커', status: 'healthy', detail: '정상'),
          SystemComponentStatus(
              id: 'core', name: 'Pi4 백엔드', status: 'healthy', detail: '정상'),
          SystemComponentStatus(
              id: 'CTRL-ESP32-ACT-01',
              name: 'ESP32 제어기',
              status: 'offline',
              detail: '15초 이상 heartbeat 없음'),
          SystemComponentStatus(
              id: 'sqlite',
              name: 'SQLite',
              status: 'healthy',
              detail: '무결성 정상'),
        ],
      ),
    );
    _emit();
  }

  // ---- core 0.6.0 task kinds (laptop rehearsal only) ----

  @override
  Future<void> demoVoiceCommand() async {
    if (!_current.connected) return;
    await _runAutomatically(_controlTask(_nextId('voice'),
        deviceId: 'LED-LIVING-01',
        action: 'set_switch',
        args: {'on': true},
        origin: 'voice_parser',
        sourceNodeId: 'NODE-RPI5-02',
        aiDevice: 'LIGHT',
        aiAction: 'TURN_ON',
        state: HomeTaskState.ready));
  }

  static const _aiProposals = <({
    String deviceId,
    String action,
    Map<String, dynamic> args,
    String aiDevice,
    String aiAction
  })>[
    (
      deviceId: 'LED-AIRCON-01',
      action: 'set_switch',
      args: {'on': true},
      aiDevice: 'AIR_CONDITIONER',
      aiAction: 'TURN_ON'
    ),
    (
      deviceId: 'SERVO-WINDOW-01',
      action: 'set_position',
      args: {'position_degrees': 90},
      aiDevice: 'WINDOW',
      aiAction: 'OPEN'
    ),
  ];

  @override
  Future<void> demoAiProposal() async {
    if (!_current.connected) return;
    final pick = _aiProposals[_aiProposalIndex++ % _aiProposals.length];
    final task = _controlTask(_nextId('llm'),
        deviceId: pick.deviceId,
        action: pick.action,
        args: pick.args,
        origin: 'llm',
        sourceNodeId: 'NODE-RPI5-01',
        aiDevice: pick.aiDevice,
        aiAction: pick.aiAction,
        state: HomeTaskState.proposed);
    _upsertTask(task);
    _current = _current.copyWith(
      contextText: 'AI가 제안했어요 · ${taskQuestion(task)}',
      llmResult: LlmResultInfo(
          eventId: _nextId('llm-result'),
          status: 'task_requested',
          taskId: task.id,
          aiIntent: 'DEVICE_CONTROL',
          aiDevice: pick.aiDevice,
          aiAction: pick.aiAction,
          completedAt: DateTime.now()),
    );
    _emit();
  }

  @override
  Future<void> demoAiClarify() async {
    if (!_current.connected) return;
    _current = _current.copyWith(
      llmResult: LlmResultInfo(
          eventId: _nextId('llm-result'),
          status: 'clarify',
          aiIntent: 'CLARIFY',
          clarificationQuestion: '어느 쪽을 열까요? 현관문인가요, 창문인가요?',
          safetyFlags: const ['AMBIGUOUS_TARGET'],
          completedAt: DateTime.now()),
    );
    _emit();
  }

  @override
  Future<void> demoEntranceDoorRequest() async {
    if (!_current.connected) return;
    _upsertTask(_controlTask(_nextId('voice'),
        deviceId: 'SERVO-DOOR-01',
        action: 'set_position',
        args: {'position_degrees': 90},
        origin: 'voice_parser',
        sourceNodeId: 'NODE-RPI5-02',
        aiDevice: 'ENTRY_DOOR',
        aiAction: 'OPEN',
        state: HomeTaskState.proposed));
    _current = _current.copyWith(contextText: '현관문 열기 요청 · 확인이 필요해요');
    _emit();
  }

  @override
  Future<void> demoUnwiredRequest() async {
    if (!_current.connected) return;
    final now = DateTime.now();
    _upsertTask(_controlTask(_nextId('voice'),
        deviceId: 'LED-GUIDE-01',
        action: 'set_switch',
        args: {'on': true},
        origin: 'voice_parser',
        sourceNodeId: 'NODE-RPI5-02',
        state: HomeTaskState.rejected,
        failure: TaskFailure(
            code: 'DEVICE_NOT_WIRED',
            message: 'LED-GUIDE-01 wiring_status is not_wired',
            occurredAt: now,
            retryAllowed: false)));
    _current = _current.copyWith(contextText: '야간 이동 조명은 아직 배선되지 않았어요');
    _emit();
  }

  @override
  Future<void> demoPhoneCommand() async {
    if (!_current.connected) return;
    await _runAutomatically(_controlTask(_nextId('phone'),
        deviceId: 'SERVO-WINDOW-01',
        action: 'set_position',
        args: {'position_degrees': 0},
        origin: 'ui',
        sourceNodeId: 'NODE-RPI4-01',
        state: HomeTaskState.ready));
  }

  @override
  Future<void> demoPartialHint() async {
    if (!_current.connected) return;
    _current = _current.copyWith(
      partialHint: PartialIntentHint(
          intent: 'VENTILATION',
          candidateDevices: const ['FAN-01', 'SERVO-WINDOW-01'],
          confidence: 0.77,
          observedAt: DateTime.now()),
    );
    _emit();
    await Future<void>.delayed(const Duration(milliseconds: 1800));
    if (_disposed) return;
    _current = _current.copyWith(clearPartialHint: true);
    _emit();
  }

  @override
  Future<void> demoMeetingStarted() async {
    if (!_current.connected) return;
    if (_quietDisputed) return;  // like core: a disputed context does not turn quiet mode on again
    _current = _current.copyWith(
      zone: HomeZone.desk,
      contextText: '책상에서 화상회의 중 · 조용 모드를 켜요',
      activity: ActivityContext(
          activity: 'MEETING',
          audioContext: 'ZOOM_SPEAKER',
          quietRequired: true,
          transition: 'ACTIVATED',
          zone: 'desk',
          confidence: 0.84,
          observedAt: DateTime.now()),
    );
    _emit();
    await _runAutomatically(_scenarioTask(
        _nextId('context'), 'quiet_mode_on', HomeTaskState.ready,
        origin: 'context_rule', sourceNodeId: 'NODE-RPI4-01'));
  }

  @override
  Future<void> demoMeetingEnded() async {
    if (!_current.connected) return;
    if (_quietDisputed) {
      _quietDisputed = false;  // the disputed context ended; quiet mode is already off
      return;
    }
    _current = _current.copyWith(
      contextText: '회의가 끝났어요 · 조용 모드를 해제해요',
      activity: ActivityContext(
          activity: 'WORKING',
          audioContext: 'QUIET',
          quietRequired: false,
          transition: 'DEACTIVATED',
          zone: 'desk',
          confidence: 0.8,
          observedAt: DateTime.now()),
    );
    _emit();
    await _runAutomatically(_scenarioTask(
        _nextId('context'), 'quiet_mode_off', HomeTaskState.ready,
        origin: 'context_rule', sourceNodeId: 'NODE-RPI4-01'));
  }

  @override
  Future<void> triggerSafetyAlert() async {
    final now = DateTime.now();
    _current = _current.copyWith(
      safetyAlert: true,
      contextText: 'P0 안전 경고 · 즉시 확인하세요',
      safetyDetail: SafetyAlertDetail(
        alertId: 'mock-alert',
        traceId: 'mock-trace',
        type: 'manual_test',
        zone: 'living',
        summary: '시연용 안전 위험을 감지했습니다',
        raisedAt: now,
        automaticActions: const ['모든 경고 표시 켜기'],
      ),
    );
    _emit();
  }

  @override
  Future<void> acknowledgeSafetyAlert() async {
    _current = _current.copyWith(
      safetyAlert: false,
      contextText: '안전 경고를 확인했습니다',
      clearSafetyDetail: true,
    );
    _emit();
  }

  @override
  Future<void> setPrivacyMode(PrivacyMode mode) async {
    final now = DateTime.now();
    _current = _current.copyWith(
      privacy: PrivacyState(
        configured: true,
        mode: mode,
        cameraAllowed:
            mode != PrivacyMode.cameraBlocked && mode != PrivacyMode.private,
        microphoneAllowed:
            mode != PrivacyMode.audioBlocked && mode != PrivacyMode.private,
        recording: false,
        localProcessing: true,
        changedAt: now,
      ),
      lastUpdated: now,
    );
    _emit();
  }

  @override
  Future<PhonePairingOffer> requestPhonePairing() async {
    if (!_current.connected) {
      throw const PhonePairingException('Pi4에 연결돼 있지 않아요.');
    }
    await Future<void>.delayed(const Duration(milliseconds: 300));
    // Demo only: a documentation address (192.0.2.0/24) and a code that no server accepts.
    final expires = DateTime.now().add(const Duration(minutes: 10));
    const name = 'HomeMind 백엔드 (데모)';
    const url = 'http://192.0.2.10:8000';
    return PhonePairingOffer(
      qrPayload: jsonEncode({
        'version': 1,
        'device_id': 'NODE-RPI4-01',
        'device_name': name,
        'base_url': url,
        'pairing_code': 'DEMO-0001',
        'nonce': 'v1.${expires.millisecondsSinceEpoch ~/ 1000}.demo',
        'expires_at': expires.toUtc().toIso8601String(),
      }),
      pairingCode: 'DEMO-0001',
      deviceName: name,
      baseUrl: url,
      expiresAt: expires,
      simulated: true,
    );
  }

  // Demo only: phones that no Mobile API knows about.
  final List<PairedPhone> _demoPhones = [
    PairedPhone(
      phoneId: 'de00a001',
      deviceName: 'Galaxy S24 (데모)',
      pairedAt: DateTime.now().subtract(const Duration(days: 1, hours: 2)),
      lastSeenAt: DateTime.now().subtract(const Duration(minutes: 3)),
    ),
    PairedPhone(
      phoneId: 'de00a002',
      deviceName: 'iPhone 15 (데모)',
      pairedAt: DateTime.now().subtract(const Duration(hours: 5)),
      lastSeenAt: null,
    ),
  ];

  @override
  Future<PairedPhones> requestPairedPhones() async {
    if (!_current.connected) {
      throw const PhonePairingException('Pi4에 연결돼 있지 않아요.');
    }
    await Future<void>.delayed(const Duration(milliseconds: 200));
    return PairedPhones(phones: List.unmodifiable(_demoPhones), simulated: true);
  }

  @override
  Future<PairedPhones> unpairPhone(String phoneId) async {
    if (!_current.connected) {
      throw const PhonePairingException('Pi4에 연결돼 있지 않아요.');
    }
    await Future<void>.delayed(const Duration(milliseconds: 200));
    final before = _demoPhones.length;
    _demoPhones.removeWhere((phone) => phone.phoneId == phoneId);
    return PairedPhones(
        phones: List.unmodifiable(_demoPhones),
        revoked: _demoPhones.length != before,
        simulated: true);
  }

  @override
  Stream<PairedPhones> get pairedPhoneUpdates => const Stream.empty();

  @override
  Future<bool> requestVoiceListen(VoiceListenAction action) async {
    if (!_current.connected) return false;
    voiceListenRequests.add(action);
    _voiceReplyTimer?.cancel();
    if (action == VoiceListenAction.start && simulateVoiceReply) {
      _voiceReplyTimer = Timer(voiceReplyDelay, () {
        if (!_disposed) demoVoiceCommand();
      });
    }
    return true;
  }

  @override
  Future<void> submitFeedback({
    required FeedbackType type,
    required FeedbackRating rating,
    String? taskId,
    String note = '',
  }) async {
    // Same as core's feedback override (context-rules.json): "상황이 달라요" while quiet mode is on.
    if (type == FeedbackType.context &&
        rating == FeedbackRating.negative &&
        _current.quietMode.active &&
        !_quietDisputed) {
      _quietDisputed = true;
      _current = _current.copyWith(contextText: '상황 판단이 틀렸다고 알려 주셨어요 · 조용 모드를 해제해요');
      _emit();
      await _runAutomatically(_scenarioTask(
          _nextId('context'), 'quiet_mode_off', HomeTaskState.ready,
          origin: 'context_rule',
          sourceNodeId: 'NODE-RPI4-01',
          rule: 'quiet_context_wrong'));
      return;
    }
    _current = _current.copyWith(
      contextText: type == FeedbackType.context
          ? '상황 판단 피드백을 저장했습니다'
          : '기기 동작 피드백을 저장했습니다',
    );
    _emit();
  }

  @override
  Future<void> setConnected(bool connected) async {
    final now = DateTime.now();
    _current = _current.copyWith(
      connected: connected,
      contextText: connected ? 'Pi4 가상 연결을 복구했습니다' : 'Pi4 연결 끊김 · 재연결 대기 중',
      connectionDetail: connected ? '가상 데이터' : '재연결 대기',
      devices: _current.devices
          .map((device) => device.copyWith(
              online: connected,
              lastSeenAt: connected ? now : device.lastSeenAt))
          .toList(growable: false),
      systemStatus: HomeSystemStatus(
        overallStatus: connected ? 'healthy' : 'offline',
        storageUsedPercent: _current.systemStatus.storageUsedPercent,
        warningThresholdPercent: 70,
        storageWarning: false,
        databaseBytes: _current.systemStatus.databaseBytes,
        generatedAt: now,
        timeValid: _current.systemStatus.timeValid,
        components: _current.systemStatus.components,
      ),
    );
    _emit();
  }

  @override
  Future<void> dispose() async {
    _disposed = true;
    _voiceReplyTimer?.cancel();
    await _controller.close();
  }
}
