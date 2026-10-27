import 'dart:async';
import 'dart:convert';
import 'dart:math';

import 'package:flutter/foundation.dart';
import 'package:mqtt_client/mqtt_client.dart';

import 'device_catalog.dart';
import 'home_mind_data_source.dart';
import 'models.dart';
import 'mqtt_auth.dart';
import 'mqtt_client_factory.dart';
import 'phone_pairing.dart';
import 'task_presentation.dart';
import 'voice_listen.dart';

class MqttHomeMindDataSource implements HomeMindDataSource {
  MqttHomeMindDataSource({
    this.host = '10.10.10.2',
    this.tcpPort = 1883,
    this.webSocketPort = 9001,
    this.webSocketPath = '',
    this.clientId = 'homemind-ui-laptop-01',
    this.auth = const MqttAuthConfig(),
  }) : _current = HomeSnapshot(
          connected: false,
          zone: HomeZone.unknown,
          contextText: 'Pi4 MQTT 연결을 준비하고 있습니다',
          tasks: const [],
          devices: _initialDevices,
          sensorReadings: const [],
          safetyAlert: false,
          lastUpdated: DateTime.now(),
          connectionDetail: '연결 중',
          systemStatus: _unknownSystemStatus(),
        );

  final String host;
  final int tcpPort;
  final int webSocketPort;

  /// Optional WebSocket path (for example `/mqtt`); web builds only.
  final String webSocketPath;
  final String clientId;

  /// Native login (Pi4 1884 listener); anonymous for the cable/web listeners.
  final MqttAuthConfig auth;

  static final List<DeviceStatus> _initialDevices =
      buildOfficialDeviceStatuses(online: false);

  static HomeSystemStatus _unknownSystemStatus() => HomeSystemStatus(
        overallStatus: 'offline',
        storageUsedPercent: 0,
        warningThresholdPercent: 70,
        storageWarning: false,
        databaseBytes: 0,
        generatedAt: DateTime.now(),
        components: const [],
      );

  final StreamController<HomeSnapshot> _controller =
      StreamController<HomeSnapshot>.broadcast();
  final Random _random = Random.secure();
  final Map<String, Completer<PhonePairingOffer>> _pairingWaits = {};
  final Map<String, Completer<PairedPhones>> _phonesWaits = {};
  final StreamController<PairedPhones> _phonesUpdates =
      StreamController<PairedPhones>.broadcast();
  HomeSnapshot _current;
  MqttClient? _client;
  StreamSubscription<List<MqttReceivedMessage<MqttMessage?>>>? _updates;
  Timer? _snapshotTimer;
  Timer? _reconnectTimer;
  bool _connecting = false;
  bool _disposed = false;
  bool _manualOffline = false;
  String? _activeAlertId;
  String? _activeAlertTraceId;
  String? _voiceTraceId;

  @override
  bool get isLive => true;

  @override
  Stream<HomeSnapshot> get snapshots => _controller.stream;

  @override
  HomeSnapshot get current => _current;

  bool get _isConnected =>
      _client?.connectionStatus?.state == MqttConnectionState.connected;

  @override
  Future<void> start() => _connect();

  Future<void> _connect() async {
    if (_disposed || _manualOffline || _connecting || _isConnected) return;
    final MqttLogin? login;
    try {
      login = auth.login();
    } on MqttAuthException catch (problem) {
      _setConnection(false, 'MQTT 인증 설정 오류', context: problem.message);
      if (!problem.permanent) _scheduleReconnect();
      return;
    }
    _connecting = true;
    _setConnection(false, '연결 중');

    final mqttClientId =
        '${clientId}_${DateTime.now().millisecondsSinceEpoch % 1000000}';
    final client = createHomeMindMqttClient(
      host: host,
      tcpPort: tcpPort,
      webSocketPort: webSocketPort,
      webSocketPath: webSocketPath,
      clientId: mqttClientId,
    )
      ..logging(on: false)
      ..keepAlivePeriod = 5
      ..connectTimeoutPeriod = 4000
      ..setProtocolV311()
      ..connectionMessage = MqttConnectMessage()
          .withClientIdentifier(mqttClientId)
          .authenticateAs(login?.username, login?.password)
          .startClean()
          .withWillQos(MqttQos.atMostOnce);

    _client = client;
    client.onConnected = () => _handleConnected(client);
    client.onDisconnected = () => _handleDisconnected(client);

    try {
      await client.connect();
      if (client.connectionStatus?.state != MqttConnectionState.connected) {
        throw StateError('MQTT CONNACK 실패: ${client.connectionStatus}');
      }
      if (!_current.connected) _handleConnected(client);
    } catch (_) {
      if (identical(_client, client)) {
        final code = client.connectionStatus?.returnCode;
        final refused = code == MqttConnectReturnCode.badUsernameOrPassword ||
            code == MqttConnectReturnCode.notAuthorized;
        _setConnection(false, refused ? 'MQTT 인증 거부' : '재연결 대기',
            context: refused
                ? 'Pi4 MQTT 인증 거부 · 사용자 이름(${login?.username ?? '없음'})과 '
                    '비밀번호 파일을 확인해 주세요'
                : null);
        _scheduleReconnect();
      }
    } finally {
      _connecting = false;
    }
  }

  void _handleConnected(MqttClient client) {
    if (_disposed || !identical(_client, client)) return;
    _reconnectTimer?.cancel();
    _setConnection(true, '실제 MQTT');
    client.subscribe(
      'pfn/v1/core/state/response/$clientId',
      MqttQos.atLeastOnce,
    );
    client.subscribe('pfn/v1/core/task/update', MqttQos.atLeastOnce);
    client.subscribe('pfn/v1/system/health', MqttQos.atMostOnce);
    client.subscribe('pfn/v1/system/safety/alert', MqttQos.atLeastOnce);
    client.subscribe('pfn/v1/system/privacy/state', MqttQos.atLeastOnce);
    client.subscribe('pfn/v1/device/+/telemetry', MqttQos.atMostOnce);
    // The UI runs on NODE-RPI5-01, an allowed subscriber of both topics.
    client.subscribe('pfn/v1/ai/perception', MqttQos.atLeastOnce);
    client.subscribe('pfn/v1/ai/llm/result', MqttQos.atLeastOnce);
    // Phone pairing offers carry a one-time code; the ACL lets only NODE-RPI5-01 read them.
    client.subscribe(kPairingOfferTopic, MqttQos.atLeastOnce);
    // Paired phone names: also readable by NODE-RPI5-01 only.
    client.subscribe(kPhonesStateTopic, MqttQos.atLeastOnce);

    _updates?.cancel();
    final updates = client.updates;
    if (updates != null) {
      _updates = updates.listen(_handleUpdates);
    }
    _snapshotTimer?.cancel();
    _snapshotTimer = Timer.periodic(
      const Duration(seconds: 5),
      (_) => requestSnapshot(),
    );
    unawaited(requestSnapshot());
  }

  void _handleDisconnected(MqttClient client) {
    if (_disposed || !identical(_client, client)) return;
    _snapshotTimer?.cancel();
    _setConnection(false, _manualOffline ? '사용자가 연결 해제' : '재연결 대기');
    if (!_manualOffline) _scheduleReconnect();
  }

  void _scheduleReconnect() {
    if (_disposed || _manualOffline || _reconnectTimer?.isActive == true) {
      return;
    }
    _reconnectTimer = Timer(const Duration(seconds: 3), _connect);
  }

  /// [context] replaces the offline home text (auth problems); texts that
  /// start with 'Pi4 MQTT' are replaced again once connected.
  void _setConnection(bool connected, String detail, {String? context}) {
    _current = _current.copyWith(
      connected: connected,
      connectionDetail: detail,
      contextText: connected
          ? (_current.contextText.startsWith('Pi4 MQTT')
              ? 'Pi4의 실제 상태를 불러오는 중입니다'
              : _current.contextText)
          : context ?? 'Pi4 연결 끊김 · 자동 재연결을 기다립니다',
      devices: connected
          ? _current.devices
          : _current.devices
              .map((device) => device.copyWith(online: false))
              .toList(growable: false),
      lastUpdated: DateTime.now(),
    );
    _emit();
  }

  void _handleUpdates(List<MqttReceivedMessage<MqttMessage?>> messages) {
    for (final received in messages) {
      final message = received.payload;
      if (message is! MqttPublishMessage) continue;
      final payload = MqttPublishPayload.bytesToStringAsString(
        message.payload.message,
      );
      debugHandleMessage(received.topic, payload);
    }
  }

  @visibleForTesting
  void debugHandleMessage(String topic, String payload) {
    try {
      final decoded = jsonDecode(payload);
      if (decoded is! Map) return;
      final data = Map<String, dynamic>.from(decoded);
      if (topic.startsWith('pfn/v1/core/state/response/')) {
        _applyStateResponse(data);
      } else if (topic == 'pfn/v1/core/task/update') {
        final task = data['task'];
        if (task is Map) {
          _upsertTask(
            _taskFromJson(
              Map<String, dynamic>.from(task),
              fallbackTraceId: data['trace_id']?.toString() ?? '',
            ),
          );
          _applyTaskEffects(_current.tasks.first);
          final origin = _current.tasks.first.originKind;
          if (origin == TaskOrigin.voice || origin == TaskOrigin.ai) {
            // the final utterance became a task: the "preparing" hint is done
            _current = _current.copyWith(clearPartialHint: true);
          }
          _emit();
        }
      } else if (topic == 'pfn/v1/ai/perception') {
        final hint = _partialHintFromJson(data);
        if (hint != null) {
          _current = _current.copyWith(partialHint: hint);
          _emit();
          return;
        }
        final activity = _activityFromJson(data);
        if (activity != null) {
          _current = _current.copyWith(
            activity: activity,
            lastUpdated: DateTime.now(),
          );
          _emit();
        }
      } else if (topic == 'pfn/v1/ai/llm/result') {
        final result = _llmResultFromJson(data);
        if (result != null) {
          _current = _current.copyWith(
            llmResult: result,
            lastUpdated: DateTime.now(),
          );
          _emit();
        }
      } else if (topic == 'pfn/v1/system/safety/alert') {
        _activeAlertId = data['alert_id']?.toString();
        _activeAlertTraceId = data['trace_id']?.toString();
        final raisedAt = (data['raised_at_ms'] as num?)?.toInt();
        _current = _current.copyWith(
          safetyAlert: true,
          contextText: data['summary']?.toString() ?? 'P0 안전 경고',
          safetyDetail: SafetyAlertDetail(
            alertId: _activeAlertId ?? '',
            traceId: _activeAlertTraceId ?? '',
            type: data['alert_type']?.toString() ?? 'unknown',
            zone: data['zone']?.toString() ?? 'unknown',
            summary: data['summary']?.toString() ?? 'P0 안전 경고',
            raisedAt: raisedAt == null
                ? DateTime.now()
                : DateTime.fromMillisecondsSinceEpoch(raisedAt),
            automaticActions: (data['automatic_actions'] as List? ?? const [])
                .map((value) => value is Map
                    ? (value['action'] ?? value['description'] ?? value)
                        .toString()
                    : value.toString())
                .toList(growable: false),
          ),
        );
        _emit();
      } else if (topic == kPairingOfferTopic) {
        _applyPairingOffer(data);
      } else if (topic == kPhonesStateTopic) {
        _applyPhonesState(data);
      } else if (topic == 'pfn/v1/system/privacy/state') {
        _applyPrivacyState(data);
      } else if (topic == 'pfn/v1/system/health') {
        _applySystemHealth(data);
      } else if (topic.startsWith('pfn/v1/device/') &&
          topic.endsWith('/telemetry')) {
        _applyTelemetry(data);
      }
    } catch (_) {
      // A malformed external message must not crash the UI.
    }
  }

  void _applyStateResponse(Map<String, dynamic> data) {
    final tasks = <HomeTask>[];
    for (final item in (data['tasks'] as List? ?? const [])) {
      if (item is Map) {
        tasks.add(_taskFromJson(Map<String, dynamic>.from(item)));
      }
    }

    final nodeOnline = <String, bool>{};
    final nodeLastSeen = <String, DateTime>{};
    for (final item in (data['devices'] as List? ?? const [])) {
      if (item is Map) {
        final node = Map<String, dynamic>.from(item);
        nodeOnline[node['node_id']?.toString() ?? ''] =
            node['status']?.toString() == 'online';
        final lastSeen = (node['last_seen_at_ms'] as num?)?.toInt();
        if (lastSeen != null && lastSeen > 0) {
          nodeLastSeen[node['node_id']?.toString() ?? ''] =
              DateTime.fromMillisecondsSinceEpoch(lastSeen);
        }
      }
    }

    final physicalDevices = _current.devices.map((device) {
      final controller = device.controllerNodeId;
      return device.copyWith(
        online: nodeOnline[controller] ?? false,
        lastSeenAt: nodeLastSeen[controller] ?? device.lastSeenAt,
      );
    }).toList(growable: false);

    final worldState = data['world_state'] as List? ?? const [];
    final zone = _zoneFromWorldState(worldState);
    final activity = _activityFromWorldState(worldState);
    final safety = data['safety'];
    var safetyActive = false;
    SafetyAlertDetail? nextSafetyDetail = _current.safetyDetail;
    if (safety is Map) {
      final map = Map<String, dynamic>.from(safety);
      safetyActive = ((map['active_count'] as num?)?.toInt() ?? 0) > 0;
      final alerts = map['active_alerts'];
      if (alerts is List && alerts.isNotEmpty && alerts.first is Map) {
        final alert = Map<String, dynamic>.from(alerts.first as Map);
        _activeAlertId = alert['alert_id']?.toString();
        _activeAlertTraceId = alert['trace_id']?.toString();
        final raisedAt = (alert['raised_at_ms'] as num?)?.toInt();
        nextSafetyDetail = SafetyAlertDetail(
          alertId: _activeAlertId ?? '',
          traceId: _activeAlertTraceId ?? '',
          type: alert['alert_type']?.toString() ?? 'unknown',
          zone: alert['zone']?.toString() ?? 'unknown',
          summary: alert['summary']?.toString() ?? 'P0 안전 경고',
          raisedAt: raisedAt == null
              ? DateTime.now()
              : DateTime.fromMillisecondsSinceEpoch(raisedAt),
          automaticActions: (alert['automatic_actions'] as List? ?? const [])
              .map((value) => value.toString())
              .toList(growable: false),
        );
      }
    }

    final privacy = data['privacy'];
    final nextPrivacy = privacy is Map
        ? _privacyFromJson(Map<String, dynamic>.from(privacy))
        : _current.privacy;

    var nextSystemStatus = data['system_health'] is Map
        ? _systemStatusFromJson(
            Map<String, dynamic>.from(data['system_health'] as Map))
        : _current.systemStatus;
    final clock = data['clock'];
    if (clock is Map) {
      final clockData = Map<String, dynamic>.from(clock);
      nextSystemStatus = nextSystemStatus.copyWith(
        timeValid: clockData['valid'] == true,
        bootUptimeMs: (clockData['uptime_ms'] as num?)?.toInt() ?? 0,
      );
    }

    _current = _current.copyWith(
      connected: true,
      connectionDetail: '실제 MQTT',
      zone: zone,
      tasks: tasks,
      devices: physicalDevices,
      safetyAlert: safetyActive,
      contextText: _contextFor(tasks, zone, safetyActive),
      lastUpdated: DateTime.now(),
      systemStatus: nextSystemStatus,
      privacy: nextPrivacy,
      safetyDetail: nextSafetyDetail,
      clearSafetyDetail: !safetyActive,
      activity: activity,
    );
    if (tasks.isNotEmpty) _applyTaskEffects(tasks.first);
    _emit();
  }

  HomeZone _zoneFromWorldState(List<dynamic> worldState) {
    Map<String, dynamic>? latest;
    for (final item in worldState) {
      if (item is! Map) continue;
      final state = Map<String, dynamic>.from(item);
      final updated = (state['updated_at_ms'] as num?)?.toInt() ?? 0;
      final currentUpdated = (latest?['updated_at_ms'] as num?)?.toInt() ?? -1;
      if (updated > currentUpdated) latest = state;
    }
    if (latest == null) return _current.zone;
    final key = latest['state_key']?.toString() ?? '';
    // core 0.6.0 stores `{zone}.{observation_type}`; older keys used
    // `{type}:{zone}`.
    final rawZone = key.contains(':')
        ? key.split(':').last
        : key.contains('.')
            ? key.split('.').first
            : '';
    return switch (rawZone) {
      'entrance' => HomeZone.entrance,
      'living' || 'living_room' => HomeZone.living,
      'bed' || 'bedroom' => HomeZone.bed,
      'bathroom' => HomeZone.bathroom,
      'desk' => HomeZone.desk,
      'away' => HomeZone.away,
      _ => _current.zone,
    };
  }

  /// Newest `{zone}.ACTIVITY_CONTEXT` row; keeps a newer live perception.
  ActivityContext? _activityFromWorldState(List<dynamic> worldState) {
    ActivityContext? best;
    for (final item in worldState) {
      if (item is! Map) continue;
      final state = Map<String, dynamic>.from(item);
      final key = state['state_key']?.toString() ?? '';
      if (!key.endsWith('.ACTIVITY_CONTEXT')) continue;
      final value = state['value'];
      if (value is! Map) continue;
      final candidate = _activityFromAttributes(
        Map<String, dynamic>.from(value),
        zone: key.substring(0, key.length - '.ACTIVITY_CONTEXT'.length),
        confidence: (state['confidence'] as num?)?.toDouble() ?? 0,
        observedAtMs: (state['observed_at_ms'] as num?)?.toInt(),
      );
      if (candidate == null) continue;
      final current = best;
      if (current == null || candidate.observedAt.isAfter(current.observedAt)) {
        best = candidate;
      }
    }
    final live = _current.activity;
    if (best == null) return live;
    if (live != null && live.observedAt.isAfter(best.observedAt)) return live;
    return best;
  }

  /// Only a DETECTED, non-executable hint for an actual intent is shown ("preparing").
  PartialIntentHint? _partialHintFromJson(Map<String, dynamic> data) {
    if (data['observation_type'] != 'PARTIAL_INTENT_HINT') return null;
    final attributes = data['attributes'];
    if (attributes is! Map ||
        attributes['result'] != 'DETECTED' ||
        attributes['execution_allowed'] != false ||
        attributes['intent'] == 'NO_ACTION' ||
        attributes['intent'] is! String) {
      return null;
    }
    final observed = (data['observed_at_ms'] as num?)?.toInt();
    return PartialIntentHint(
      intent: attributes['intent'] as String,
      candidateDevices: (attributes['candidate_devices'] as List? ?? const [])
          .map((value) => value.toString())
          .toList(growable: false),
      confidence: (data['confidence'] as num?)?.toDouble() ?? 0,
      observedAt: observed == null
          ? DateTime.now()
          : DateTime.fromMillisecondsSinceEpoch(observed),
    );
  }

  ActivityContext? _activityFromJson(Map<String, dynamic> data) {
    if (data['observation_type'] != 'ACTIVITY_CONTEXT') return null;
    final attributes = data['attributes'];
    if (attributes is! Map) return null;
    return _activityFromAttributes(
      Map<String, dynamic>.from(attributes),
      zone: data['zone']?.toString() ?? 'unknown',
      confidence: (data['confidence'] as num?)?.toDouble() ?? 0,
      observedAtMs: (data['observed_at_ms'] as num?)?.toInt(),
      eventId: data['event_id']?.toString(),
    );
  }

  ActivityContext? _activityFromAttributes(
    Map<String, dynamic> attributes, {
    required String zone,
    required double confidence,
    required int? observedAtMs,
    String? eventId,
  }) {
    final activity = attributes['activity']?.toString();
    if (activity == null) return null;
    return ActivityContext(
      activity: activity,
      audioContext: attributes['audio_context']?.toString() ?? 'UNKNOWN',
      quietRequired: attributes['quiet_required'] == true,
      transition: attributes['transition']?.toString() ?? 'UNCHANGED',
      zone: zone,
      confidence: confidence,
      observedAt: observedAtMs == null
          ? DateTime.now()
          : DateTime.fromMillisecondsSinceEpoch(observedAtMs),
      eventId: eventId,
    );
  }

  LlmResultInfo? _llmResultFromJson(Map<String, dynamic> data) {
    final status = data['status']?.toString();
    if (status == null) return null;
    final completed = (data['completed_at_ms'] as num?)?.toInt();
    String? text(String key) => data[key]?.toString();

    return LlmResultInfo(
      eventId: data['event_id']?.toString() ?? '',
      status: status,
      completedAt: completed == null
          ? DateTime.now()
          : DateTime.fromMillisecondsSinceEpoch(completed),
      taskId: text('task_id'),
      aiIntent: text('ai_intent'),
      aiDevice: text('ai_device'),
      aiAction: text('ai_action'),
      clarificationQuestion: text('clarification_question'),
      safetyFlags: (data['safety_flags'] as List? ?? const [])
          .map((flag) => flag.toString())
          .toList(growable: false),
    );
  }

  String _contextFor(
    List<HomeTask> tasks,
    HomeZone zone,
    bool safetyActive,
  ) {
    if (safetyActive) return 'P0 안전 경고 · 즉시 확인하세요';
    if (tasks.isEmpty) return 'Pi4 연결됨 · 현재 위치 ${zone.label}';
    final task = tasks.first;
    return '${task.title} · ${task.state.label}';
  }

  HomeTask _taskFromJson(
    Map<String, dynamic> task, {
    String fallbackTraceId = '',
  }) {
    final intent = task['intent']?.toString() ?? 'unknown';
    final failureData = task['failure'];
    TaskFailure? failure;
    if (failureData is Map) {
      final map = Map<String, dynamic>.from(failureData);
      final occurred = (map['occurred_at_ms'] as num?)?.toInt();
      failure = TaskFailure(
        code: map['code']?.toString() ?? 'COMMAND_FAILED',
        message: map['message']?.toString() ?? '장치가 명령을 실행하지 못했습니다.',
        occurredAt: occurred == null
            ? DateTime.now()
            : DateTime.fromMillisecondsSinceEpoch(occurred),
        retryAllowed: map['retry_allowed'] != false,
      );
    } else if (_taskState(task['state']?.toString()) == HomeTaskState.failed) {
      failure = TaskFailure(
        code: 'COMMAND_FAILED',
        message: 'Pi4가 작업 실패를 알렸습니다. 장치 연결과 ACK 기록을 확인하세요.',
        occurredAt: DateTime.now(),
      );
    }
    final rawParameters = task['parameters'];
    final parameters = rawParameters is Map
        ? Map<String, dynamic>.from(rawParameters)
        : <String, dynamic>{};
    final text = describeTask(intent, parameters);
    return HomeTask(
      id: task['task_id']?.toString() ?? '',
      title: text.title,
      detail: text.detail,
      state: _taskState(task['state']?.toString()),
      priority: (task['priority'] as num?)?.toInt() ?? 0,
      revision: (task['revision'] as num?)?.toInt() ?? 1,
      traceId: task['trace_id']?.toString() ?? fallbackTraceId,
      actionToken: task['action_token']?.toString() ?? '',
      intent: intent,
      targetDevice: task['target_device']?.toString() ?? '',
      confidence: (task['confidence'] as num?)?.toDouble() ?? 0,
      updatedAt: task['updated_at_ms'] is num
          ? DateTime.fromMillisecondsSinceEpoch(
              (task['updated_at_ms'] as num).toInt())
          : null,
      failure: failure,
      origin: parameters['origin']?.toString() ?? '',
      sourceNodeId: task['source_node_id']?.toString() ?? '',
      parameters: parameters,
    );
  }

  void _applyPrivacyState(Map<String, dynamic> data) {
    _current = _current.copyWith(
      privacy: _privacyFromJson(data),
      lastUpdated: DateTime.now(),
    );
    _emit();
  }

  PrivacyState _privacyFromJson(Map<String, dynamic> data) {
    final changedAt = (data['changed_at_ms'] as num?)?.toInt();
    return PrivacyState(
      configured: data['configured'] != false && data['mode'] != null,
      mode: _privacyMode(data['mode']?.toString()),
      cameraAllowed: data['camera_allowed'] != false,
      microphoneAllowed: data['microphone_allowed'] != false,
      recording: data['recording'] == true,
      localProcessing: data['local_processing'] != false,
      changedAt: changedAt == null || changedAt <= 0
          ? null
          : DateTime.fromMillisecondsSinceEpoch(changedAt),
    );
  }

  PrivacyMode _privacyMode(String? value) => switch (value) {
        'camera_blocked' => PrivacyMode.cameraBlocked,
        'audio_blocked' => PrivacyMode.audioBlocked,
        'private' => PrivacyMode.private,
        _ => PrivacyMode.normal,
      };

  HomeTaskState _taskState(String? value) => switch (value) {
        'ready' => HomeTaskState.ready,
        'running' => HomeTaskState.running,
        'deferred' => HomeTaskState.deferred,
        'completed' => HomeTaskState.completed,
        'failed' => HomeTaskState.failed,
        'rejected' => HomeTaskState.rejected,
        'cancelled' => HomeTaskState.cancelled,
        _ => HomeTaskState.proposed,
      };

  void _upsertTask(HomeTask next) {
    final tasks = [..._current.tasks];
    final index = tasks.indexWhere((task) => task.id == next.id);
    if (index == -1) {
      tasks.insert(0, next);
    } else {
      tasks[index] = next;
      final updated = tasks.removeAt(index);
      tasks.insert(0, updated);
    }
    _current = _current.copyWith(
      tasks: tasks,
      contextText: '${next.title} · ${next.state.label}',
      lastUpdated: DateTime.now(),
    );
  }

  void _applyTaskEffects(HomeTask task) {
    final devices = applyTaskEffects(_current.devices, task);
    if (identical(devices, _current.devices)) return;
    _current = _current.copyWith(devices: devices);
  }

  void _applySystemHealth(Map<String, dynamic> data) {
    final status = _systemStatusFromJson(data).copyWith(
      timeValid: _current.systemStatus.timeValid,
      bootUptimeMs: _current.systemStatus.bootUptimeMs,
    );
    _current = _current.copyWith(
      systemStatus: status,
      connectionDetail: switch (status.overallStatus) {
        'offline' => 'Pi4 상태 이상',
        'degraded' => '일부 장치 점검 필요',
        _ => '실제 MQTT',
      },
      lastUpdated: DateTime.now(),
    );
    _emit();
  }

  HomeSystemStatus _systemStatusFromJson(Map<String, dynamic> data) {
    final storage = data['storage'] is Map
        ? Map<String, dynamic>.from(data['storage'] as Map)
        : <String, dynamic>{};
    final components = <SystemComponentStatus>[];
    for (final item in (data['components'] as List? ?? const [])) {
      if (item is! Map) continue;
      final component = Map<String, dynamic>.from(item);
      final id = component['component_id']?.toString() ??
          component['id']?.toString() ??
          component['component']?.toString() ??
          component['name']?.toString() ??
          'component';
      components.add(SystemComponentStatus(
        id: id,
        name: component['display_name']?.toString() ??
            component['name']?.toString() ??
            switch (id) {
              'core' => 'Pi4 백엔드',
              'mqtt' => 'MQTT 브로커',
              'sqlite' => 'SQLite',
              _ => id,
            },
        status: component['status']?.toString() ?? 'unknown',
        detail: component['detail']?.toString() ?? '',
      ));
    }
    final generated = (data['generated_at_ms'] as num?)?.toInt();
    return HomeSystemStatus(
      overallStatus: data['overall_status']?.toString() ?? 'offline',
      storageUsedPercent: (storage['used_percent'] as num?)?.toDouble() ?? 0,
      warningThresholdPercent:
          (storage['warning_threshold_percent'] as num?)?.toDouble() ?? 70,
      storageWarning: storage['warning'] == true,
      databaseBytes: (storage['database_bytes'] as num?)?.toInt() ?? 0,
      generatedAt: generated == null
          ? DateTime.now()
          : DateTime.fromMillisecondsSinceEpoch(generated),
      components: components,
    );
  }

  void _applyTelemetry(Map<String, dynamic> data) {
    final readings = data['readings'];
    final sources = data['reading_sources'];
    if (readings is! Map || sources is! Map) return;
    final sentAt = (data['sent_at_ms'] as num?)?.toInt();
    final updatedAt = sentAt == null
        ? DateTime.now()
        : DateTime.fromMillisecondsSinceEpoch(sentAt);
    final next = [..._current.sensorReadings];
    for (final entry in readings.entries) {
      final key = entry.key.toString();
      final source = sources[key]?.toString() ?? '';
      final spec = _sensorSpec(key, entry.value, source);
      if (spec == null) continue;
      final reading = SensorReading(
        key: key,
        label: spec.label,
        value: spec.value,
        unit: spec.unit,
        sourceDeviceId: source,
        updatedAt: updatedAt,
        valid: spec.valid,
      );
      final index = next.indexWhere(
          (item) => item.key == key && item.sourceDeviceId == source);
      if (index == -1) {
        next.add(reading);
      } else {
        next[index] = reading;
      }
    }
    final sourceIds = sources.values.map((value) => value.toString()).toSet();
    _current = _current.copyWith(
      sensorReadings: next,
      devices: _current.devices.map((device) {
        if (!sourceIds.contains(device.id)) return device;
        final related = next
            .where((reading) => reading.sourceDeviceId == device.id)
            .toList();
        final pattern =
            related.where((reading) => reading.key == 'light_pattern');
        final shown = pattern.isNotEmpty
            ? pattern.last
            : related.isEmpty
                ? null
                : related.last;
        final value =
            shown == null ? device.displayValue : '${shown.value}${shown.unit}';
        return device.copyWith(
            online: true, displayValue: value, lastSeenAt: updatedAt);
      }).toList(growable: false),
      lastUpdated: DateTime.now(),
    );
    _emit();
  }

  ({String label, String value, String unit, bool valid})? _sensorSpec(
      String key, Object? raw, String source) {
    bool numberIn(double min, double max) =>
        raw is num && raw.toDouble() >= min && raw.toDouble() <= max;
    String numberValue() => raw is num
        ? (raw % 1 == 0 ? raw.toInt().toString() : raw.toStringAsFixed(1))
        : raw.toString();
    return switch (key) {
      'sound_level_db' when source == 'MIC-INDOOR-01' => (
          label: '실내 소음',
          value: numberValue(),
          unit: ' dB',
          valid: numberIn(0, 120)
        ),
      'fan_level_percent' when source == 'FAN-01' => (
          label: '화장실 환기 팬 세기',
          value: numberValue(),
          unit: '%',
          valid: numberIn(0, 100)
        ),
      'x_home_active' when source == 'SENSOR-HOME-X-01' => (
          label: 'X축 원점',
          value: raw == true ? '감지' : '미감지',
          unit: '',
          valid: raw is bool
        ),
      'y_home_active' when source == 'SENSOR-HOME-Y-01' => (
          label: 'Y축 원점',
          value: raw == true ? '감지' : '미감지',
          unit: '',
          valid: raw is bool
        ),
      'door_position_degrees' when source == 'SERVO-DOOR-01' => (
          label: '현관문',
          value: positionText(source, raw),
          unit: '',
          valid: numberIn(0, 180)
        ),
      'window_position_degrees' when source == 'SERVO-WINDOW-01' => (
          label: '창문',
          value: positionText(source, raw),
          unit: '',
          valid: numberIn(0, 180)
        ),
      'bed_curtain_position_degrees' when source == 'SERVO-BED-CURTAIN-01' => (
          label: '침대 커튼 위치',
          value: numberValue(),
          unit: '°',
          valid: numberIn(0, 180)
        ),
      'light_on' when source.startsWith('LED-') => (
          label: '조명 상태',
          value: raw == true ? '켜짐' : '꺼짐',
          unit: '',
          valid: raw is bool
        ),
      'light_pattern' when source.startsWith('LED-') => (
          label: '표시등 패턴',
          value: lightPatternLabel(raw),
          unit: '',
          valid: raw == 'off' || raw == 'steady' || raw == 'blink'
        ),
      'display_value' when source.startsWith('FND-') => (
          label: 'FND 표시값',
          value: numberValue(),
          unit: '',
          valid: raw is int && raw >= 0 && raw <= 9999
        ),
      _ => null,
    };
  }

  @override
  Future<void> requestSnapshot() async {
    if (!_isConnected) return;
    final now = DateTime.now().millisecondsSinceEpoch;
    final traceId = _uuid();
    _publish(
      'pfn/v1/core/state/request',
      {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': traceId,
        'request_id': _uuid(),
        'client_id': clientId,
        'sent_at_ms': now,
      },
      MqttQos.atLeastOnce,
    );
  }

  Future<void> _requestScenario(String intent) async {
    if (!_isConnected) return;
    final traceId = _uuid();
    _publish(
      'pfn/v1/core/task/request',
      {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': traceId,
        'task_id': _uuid(),
        'source_node_id': 'NODE-RPI5-01',
        'intent': intent,
        'target_device': 'HOME-ENV-01',
        'priority': intent == 'night_bed_exit' ? 70 : 40,
        'safety_level': 'normal',
        'interruptible': true,
        'resume_policy': 'automatic',
        'confidence': 1.0,
        'parameters': <String, dynamic>{
          'origin': 'ui',
          'source': 'manual_ui_test',
        },
        'requested_at_ms': DateTime.now().millisecondsSinceEpoch,
      },
      MqttQos.atLeastOnce,
    );
  }

  @override
  Future<void> simulateArrival() => _requestScenario('prepare_home');

  @override
  Future<void> proposeSleep() => _requestScenario('prepare_sleep');

  @override
  Future<void> simulateNightExit() => _requestScenario('night_bed_exit');

  @override
  Future<void> simulateCommandTimeout() async {}

  @override
  Future<void> simulateDeviceOffline() async {}

  @override
  Future<void> actOnTask(String taskId, TaskAction action) async {
    if (!_isConnected) return;
    HomeTask? task;
    for (final candidate in _current.tasks) {
      if (candidate.id == taskId) {
        task = candidate;
        break;
      }
    }
    if (task == null || task.actionToken.isEmpty || task.traceId.isEmpty) {
      return;
    }
    _publish(
      'pfn/v1/ui/action',
      {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': task.traceId,
        'task_id': task.id,
        'client_id': clientId,
        'action': switch (action) {
          TaskAction.approve => 'approve',
          TaskAction.reject => 'reject',
          TaskAction.cancel => 'cancel',
        },
        'expected_revision': task.revision,
        'action_token': task.actionToken,
        'reason': 'Flutter UI action',
        'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
      },
      MqttQos.atLeastOnce,
    );
  }

  @override
  Future<void> triggerSafetyAlert() async {
    if (!_isConnected) return;
    final traceId = _uuid();
    final alertId = _uuid();
    _activeAlertId = alertId;
    _activeAlertTraceId = traceId;
    _publish(
      'pfn/v1/system/safety/alert',
      {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': traceId,
        'alert_id': alertId,
        'source_node_id': 'NODE-RPI5-01',
        'alert_type': 'manual_test',
        'zone': 'living',
        'severity': 'p0',
        'summary': 'Flutter UI 수동 P0 시험 경고',
        'evidence': <String, dynamic>{'manual_test': true},
        'raised_at_ms': DateTime.now().millisecondsSinceEpoch,
        'automatic_actions': <dynamic>[],
      },
      MqttQos.atLeastOnce,
    );
  }

  @override
  Future<void> acknowledgeSafetyAlert() async {
    if (!_isConnected ||
        _activeAlertId == null ||
        _activeAlertTraceId == null) {
      return;
    }
    _publish(
      'pfn/v1/ui/safety/ack',
      {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': _activeAlertTraceId,
        'alert_id': _activeAlertId,
        'client_id': clientId,
        'action': 'dismiss_test',
        'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
      },
      MqttQos.atLeastOnce,
    );
    _current = _current.copyWith(
      safetyAlert: false,
      contextText: '안전 경고를 확인했습니다',
      clearSafetyDetail: true,
    );
    _emit();
  }

  @override
  Future<void> setPrivacyMode(PrivacyMode mode) async {
    if (!_isConnected) return;
    final traceId = _uuid();
    _publish(
      'pfn/v1/ui/privacy/set',
      {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': traceId,
        'client_id': clientId,
        'mode': mode.wireValue,
        'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
      },
      MqttQos.atLeastOnce,
    );
  }

  @override
  Future<PhonePairingOffer> requestPhonePairing() {
    if (!_isConnected) {
      return Future.error(const PhonePairingException('Pi4에 연결돼 있지 않아요.'));
    }
    final requestId = _uuid();
    final offer = debugWaitForPairing(requestId);
    _publish(
      kPairingRequestTopic,
      buildPairingRequestPayload(
        clientId: clientId,
        eventId: _uuid(),
        requestId: requestId,
        sentAtMs: DateTime.now().millisecondsSinceEpoch,
      ),
      MqttQos.atLeastOnce,
    );
    return offer;
  }

  /// Registers the wait for the offer answering [requestId] (tests call it without a broker).
  @visibleForTesting
  Future<PhonePairingOffer> debugWaitForPairing(String requestId) {
    final waiter = Completer<PhonePairingOffer>();
    _pairingWaits[requestId] = waiter;
    return waiter.future.timeout(kPairingReplyTimeout, onTimeout: () {
      _pairingWaits.remove(requestId);
      throw const PhonePairingException(
          'Pi4의 휴대폰 연결 서비스(Mobile API)가 응답하지 않아요.');
    });
  }

  /// Only answers to this screen's own requests count; nothing of the offer is logged.
  void _applyPairingOffer(Map<String, dynamic> data) {
    if (data['target_node_id'] != 'NODE-RPI5-01') return;
    final waiter = _pairingWaits.remove(data['request_id']);
    if (waiter == null || waiter.isCompleted) return;
    if (data['status'] == 'offered') {
      final offer = PhonePairingOffer.fromJson(data);
      if (offer == null) {
        waiter.completeError(
            const PhonePairingException('Pi4가 보낸 QR 정보가 올바르지 않아요.'));
      } else {
        waiter.complete(offer);
      }
    } else {
      waiter.completeError(PhonePairingException(
          pairingUnavailableMessage(data['reason']?.toString())));
    }
  }

  @override
  Future<PairedPhones> requestPairedPhones() => _requestPhones(null);

  @override
  Future<PairedPhones> unpairPhone(String phoneId) => _requestPhones(phoneId);

  @override
  Stream<PairedPhones> get pairedPhoneUpdates => _phonesUpdates.stream;

  Future<PairedPhones> _requestPhones(String? revokePhoneId) {
    if (!_isConnected) {
      return Future.error(const PhonePairingException('Pi4에 연결돼 있지 않아요.'));
    }
    final requestId = _uuid();
    final answer = debugWaitForPhones(requestId);
    _publish(
      kPhonesRequestTopic,
      buildPhonesRequestPayload(
        clientId: clientId,
        eventId: _uuid(),
        requestId: requestId,
        sentAtMs: DateTime.now().millisecondsSinceEpoch,
        revokePhoneId: revokePhoneId,
      ),
      MqttQos.atLeastOnce,
    );
    return answer;
  }

  /// Registers the wait for the phone list answering [requestId] (tests call it without a broker).
  @visibleForTesting
  Future<PairedPhones> debugWaitForPhones(String requestId) {
    final waiter = Completer<PairedPhones>();
    _phonesWaits[requestId] = waiter;
    return waiter.future.timeout(kPairingReplyTimeout, onTimeout: () {
      _phonesWaits.remove(requestId);
      throw const PhonePairingException(
          'Pi4의 휴대폰 연결 서비스(Mobile API)가 응답하지 않아요.');
    });
  }

  /// Answers to this screen's requests complete them; a list sent after a pairing goes to [pairedPhoneUpdates].
  void _applyPhonesState(Map<String, dynamic> data) {
    if (data['target_node_id'] != 'NODE-RPI5-01') return;
    final requestId = data['request_id'];
    if (requestId == null) {
      final phones = PairedPhones.fromJson(data);
      if (phones != null && !_phonesUpdates.isClosed) _phonesUpdates.add(phones);
      return;
    }
    final waiter = _phonesWaits.remove(requestId);
    if (waiter == null || waiter.isCompleted) return;
    if (data['status'] == 'ok') {
      final phones = PairedPhones.fromJson(data);
      if (phones == null) {
        waiter.completeError(
            const PhonePairingException('Pi4가 보낸 휴대폰 목록이 올바르지 않아요.'));
      } else {
        waiter.complete(phones);
      }
    } else {
      waiter.completeError(PhonePairingException(
          phonesUnavailableMessage(data['reason']?.toString())));
    }
  }

  @override
  Future<bool> requestVoiceListen(VoiceListenAction action) async {
    if (!_isConnected) return false;
    _publish(
      kVoiceListenTopic,
      debugVoiceListenPayload(action),
      MqttQos.atLeastOnce,
    );
    return true;
  }

  /// Fresh event id each press; a cancel keeps the trace of its start.
  @visibleForTesting
  Map<String, dynamic> debugVoiceListenPayload(VoiceListenAction action) {
    if (action == VoiceListenAction.start || _voiceTraceId == null) {
      _voiceTraceId = _uuid();
    }
    return buildVoiceListenPayload(
      action: action,
      clientId: clientId,
      eventId: _uuid(),
      traceId: _voiceTraceId!,
      sentAtMs: DateTime.now().millisecondsSinceEpoch,
    );
  }

  @override
  Future<void> submitFeedback({
    required FeedbackType type,
    required FeedbackRating rating,
    String? taskId,
    String note = '',
  }) async {
    if (!_isConnected) return;
    _publish(
      'pfn/v1/ui/feedback',
      debugFeedbackPayload(type: type, rating: rating, taskId: taskId, note: note),
      MqttQos.atLeastOnce,
    );
  }

  @visibleForTesting
  Map<String, dynamic> debugFeedbackPayload({
    required FeedbackType type,
    required FeedbackRating rating,
    String? taskId,
    String note = '',
  }) {
    String? traceId;
    if (taskId != null) {
      for (final task in _current.tasks) {
        if (task.id == taskId) traceId = task.traceId;
      }
    }
    return {
        'schema_version': 'pfn.v1',
        'event_id': _uuid(),
        'trace_id': traceId ?? _uuid(),
        'client_id': clientId,
        'feedback_type': type == FeedbackType.context
            ? 'context_feedback'
            : 'action_feedback',
        'task_id': taskId,
        // context feedback names the perception it disputes (core undoes quiet mode for it)
        'observation_event_id': type == FeedbackType.context
            ? _uuidOrNull(_current.activity?.eventId)
            : null,
        'rating': switch (rating) {
          FeedbackRating.positive => 'positive',
          FeedbackRating.negative => 'negative',
          FeedbackRating.unknown => 'unknown',
        },
        'note': note.length <= 512 ? note : note.substring(0, 512),
        'sent_at_ms': DateTime.now().millisecondsSinceEpoch,
      };
  }

  void _publish(
    String topic,
    Map<String, dynamic> payload,
    MqttQos qos,
  ) {
    final client = _client;
    if (client == null || !_isConnected) return;
    final builder = MqttClientPayloadBuilder()..addString(jsonEncode(payload));
    client.publishMessage(topic, qos, builder.payload!, retain: false);
  }

  static final _uuidPattern = RegExp(
      r'^[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}$');

  static String? _uuidOrNull(String? value) =>
      value != null && _uuidPattern.hasMatch(value) ? value : null;

  String _uuid() {
    final bytes = List<int>.generate(16, (_) => _random.nextInt(256));
    bytes[6] = (bytes[6] & 0x0f) | 0x40;
    bytes[8] = (bytes[8] & 0x3f) | 0x80;
    final hex =
        bytes.map((value) => value.toRadixString(16).padLeft(2, '0')).join();
    return '${hex.substring(0, 8)}-${hex.substring(8, 12)}-'
        '${hex.substring(12, 16)}-${hex.substring(16, 20)}-'
        '${hex.substring(20)}';
  }

  void _emit() {
    if (!_disposed) _controller.add(_current);
  }

  @override
  Future<void> setConnected(bool connected) async {
    if (connected) {
      _manualOffline = false;
      await _connect();
      return;
    }
    _manualOffline = true;
    _reconnectTimer?.cancel();
    _snapshotTimer?.cancel();
    _client?.disconnect();
    _setConnection(false, '사용자가 연결 해제');
  }

  @override
  Future<void> dispose() async {
    _disposed = true;
    _manualOffline = true;
    _reconnectTimer?.cancel();
    _snapshotTimer?.cancel();
    await _updates?.cancel();
    _client?.disconnect();
    await _phonesUpdates.close();
    await _controller.close();
  }
}
