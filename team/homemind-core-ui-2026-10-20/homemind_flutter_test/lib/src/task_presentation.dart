import 'device_catalog.dart';
import 'models.dart';

/// Korean wording for tasks. Pure functions so both data sources and the
/// widgets share one vocabulary (contracts/pfn.v1 scenario-rules.json v2,
/// device-registry.json v3, ai-action-map.json v1).
class TaskText {
  const TaskText(
      {required this.title, required this.detail, required this.question});
  final String title;
  final String detail;

  /// Approval question ("에어컨을 켤까요?").
  final String question;
}

const _scenarioTexts = <String, TaskText>{
  'prepare_home': TaskText(
      title: '귀가 준비',
      detail: '거실 조명과 화장실 환기 팬을 켭니다.',
      question: '귀가 준비를 시작할까요?'),
  'prepare_sleep': TaskText(
      title: '취침 준비',
      detail: '거실 조명과 환기 팬을 끄고 창문을 닫습니다.',
      question: '취침 준비를 시작할까요?'),
  'night_bed_exit': TaskText(
      title: '야간 이동 조명',
      detail: '거실 조명을 15초 동안 켜서 이동 길을 밝힙니다.',
      question: '야간 이동 조명을 켤까요?'),
  'quiet_mode_on': TaskText(
      title: '조용 모드 켜기',
      detail: '에어컨 저소음 (표시등 깜빡임)',
      question: '조용 모드를 켤까요?'),
  'quiet_mode_off': TaskText(
      title: '조용 모드 해제',
      detail: '에어컨 표시등을 원래 상태로 돌립니다.',
      question: '조용 모드를 해제할까요?'),
};

/// Quiet overlay wording per LED (set_color pattern blink / steady).
const _quietWords = <String, ({String on, String off, String ask, String undo})>{
  'LED-TV-01': (
    on: '음소거',
    off: '음소거 해제',
    ask: 'TV를 음소거할까요?',
    undo: 'TV 음소거를 해제할까요?'
  ),
  'LED-AIRCON-01': (
    on: '저소음 모드',
    off: '저소음 모드 해제',
    ask: '에어컨을 저소음 모드로 바꿀까요?',
    undo: '에어컨 저소음 모드를 해제할까요?'
  ),
  'LED-WASHER-01': (
    on: '알림 보류',
    off: '알림 재개',
    ask: '세탁기 알림을 보류할까요?',
    undo: '세탁기 알림을 다시 켤까요?'
  ),
};

/// Korean object particle (을/를) for the last syllable of [word].
String objectParticle(String word) {
  final trimmed = word.replaceAll(RegExp(r'[\s)\]]+$'), '');
  if (trimmed.isEmpty) return '를';
  final code = trimmed.codeUnitAt(trimmed.length - 1);
  if (code >= 0xAC00 && code <= 0xD7A3) {
    return (code - 0xAC00) % 28 == 0 ? '를' : '을';
  }
  const digitsWithBatchim = '0136780';
  final last = trimmed[trimmed.length - 1];
  if (RegExp(r'[0-9]').hasMatch(last)) {
    return digitsWithBatchim.contains(last) ? '을' : '를';
  }
  return '를';
}

String _withObject(String word) => '$word${objectParticle(word)}';

num? _number(Object? value) => value is num ? value : num.tryParse('$value');

String _degrees(num value) =>
    value % 1 == 0 ? '${value.toInt()}°' : '${value.toStringAsFixed(1)}°';

/// 'open' | 'closed' | null for a servo angle, from registry positions.
String? positionRef(String deviceId, Object? degrees) {
  final device = officialDeviceById(deviceId);
  final value = _number(degrees);
  if (device == null || value == null) return null;
  if (device.openDegrees != null && value == device.openDegrees) return 'open';
  if (device.closedDegrees != null && value == device.closedDegrees) {
    return 'closed';
  }
  return null;
}

/// 열림 / 닫힘 for a registered position (registry v4: open/close motors), else the raw number.
String positionText(String deviceId, Object? degrees) {
  final ref = positionRef(deviceId, degrees);
  if (ref == 'open') return '열림';
  if (ref == 'closed') return '닫힘';
  final value = _number(degrees);
  return value == null ? '$degrees' : _degrees(value);
}

TaskText describeTask(String intent, Map<String, dynamic> parameters) {
  final scenario = _scenarioTexts[intent];
  if (scenario != null) return scenario;
  if (intent != 'device_control') {
    return TaskText(
        title: intent.isEmpty ? '작업' : intent,
        detail: 'Pi4가 요청한 작업입니다.',
        question: '${intent.isEmpty ? '이 작업' : intent}을 실행할까요?');
  }

  final deviceId = parameters['target_device_id']?.toString() ?? '';
  final action = parameters['action']?.toString() ?? '';
  final rawArgs = parameters['parameters'];
  final args =
      rawArgs is Map ? Map<String, dynamic>.from(rawArgs) : <String, dynamic>{};
  final device = officialDeviceById(deviceId);
  final short = device?.shortName ?? (deviceId.isEmpty ? '기기' : deviceId);
  final detail = device == null
      ? '등록표에 없는 기기 · $deviceId'
      : '${device.location} · ${device.name} · $deviceId';

  TaskText text(String title, String question) =>
      TaskText(title: title, detail: detail, question: question);

  switch (action) {
    case 'set_switch':
      final on = args['on'] == true;
      return on
          ? text('$short 켜기', '${_withObject(short)} 켤까요?')
          : text('$short 끄기', '${_withObject(short)} 끌까요?');
    case 'set_position':
      final degrees = _number(args['position_degrees']);
      final ref = positionRef(deviceId, degrees);
      if (ref == 'open') {
        return text('$short 열기', '${_withObject(short)} 열까요?');
      }
      if (ref == 'closed') {
        return text('$short 닫기', '${_withObject(short)} 닫을까요?');
      }
      final angle = degrees == null ? '?' : _degrees(degrees);
      return text('$short $angle로 움직이기', '${_withObject(short)} $angle로 움직일까요?');
    case 'set_color':
      final color = args['color']?.toString() ?? '';
      final pattern = args['pattern']?.toString() ?? 'steady';
      if (color == 'off') {
        return text('$short 끄기', '${_withObject(short)} 끌까요?');
      }
      final words = _quietWords[deviceId];
      if (pattern == 'blink') {
        return words == null
            ? text('$short 깜빡임 표시', '$short 표시등을 깜빡일까요?')
            : text('$short ${words.on}(깜빡임)', words.ask);
      }
      return words == null
          ? text('$short 깜빡임 해제', '$short 표시등 깜빡임을 끌까요?')
          : text('$short ${words.off}', words.undo);
    case 'set_level':
      final level = _number(args['level_percent']);
      final value = level == null ? '?' : '${level.round()}%';
      return text('$short 세기 $value', '$short 세기를 $value로 바꿀까요?');
    case 'set_number':
      final value = _number(args['value']);
      final shown = value == null ? '?' : '${value.toInt()}';
      return text('$short $shown 표시', '$short에 $shown${objectParticle(shown)} 표시할까요?');
    default:
      return text('$short 제어', '${_withObject(short)} 제어할까요?');
  }
}

String taskQuestion(HomeTask task) =>
    describeTask(task.intent, task.parameters).question;

/// Short Korean label for task.failure.code (task-policy.json rejection_codes
/// and core failure codes).
String failureShortLabel(String code) => switch (code) {
      'DEVICE_NOT_WIRED' => '미배선 기기',
      'CAPABILITY_NOT_ALLOWED' => '지원하지 않는 동작',
      'SOURCE_NOT_ALLOWED' => '허용되지 않은 요청 출처',
      'POSITION_NOT_REGISTERED' => '등록되지 않은 각도',
      'COLOR_NOT_SUPPORTED' => '지원하지 않는 색',
      'PARAMETERS_INVALID' => '잘못된 요청 값',
      'DEVICE_UNKNOWN' => '등록되지 않은 기기',
      'ACK_TIMEOUT' => '기기 응답 없음',
      'COMMAND_FAILED' => '기기 실행 실패',
      'PROPOSAL_EXPIRED' => '승인 시간 지남',
      _ => code,
    };

/// One Korean sentence explaining the failure; falls back to core's message.
String failureExplanation(TaskFailure failure) => switch (failure.code) {
      'DEVICE_NOT_WIRED' => '이 기기는 아직 배선되지 않아 실행하지 않았어요.',
      'CAPABILITY_NOT_ALLOWED' => '이 기기가 할 수 없는 동작이라 실행하지 않았어요.',
      'SOURCE_NOT_ALLOWED' => '허용되지 않은 장치에서 온 요청이라 막았어요.',
      'POSITION_NOT_REGISTERED' => '등록된 열림·닫힘 각도가 아니라 움직이지 않았어요.',
      'COLOR_NOT_SUPPORTED' => '이 표시등이 낼 수 없는 색이라 바꾸지 않았어요.',
      'PARAMETERS_INVALID' => '요청 값이 올바르지 않아 실행하지 않았어요.',
      'DEVICE_UNKNOWN' => '등록표에 없는 기기라 실행하지 않았어요.',
      'ACK_TIMEOUT' => '3초씩 3번 기다렸지만 기기가 응답하지 않았어요.',
      'PROPOSAL_EXPIRED' => '승인을 기다리는 시간이 지나 실행하지 않고 닫았어요. 필요하면 다시 요청해 주세요.',
      _ => failure.message,
    };

String _switchDisplay(bool on) => on ? 'ON' : 'OFF';

/// Approximate device display after a task, until telemetry confirms it.
List<DeviceStatus> applyTaskEffects(List<DeviceStatus> devices, HomeTask task) {
  final completed = task.state == HomeTaskState.completed;
  final started = completed ||
      task.state == HomeTaskState.running ||
      task.state == HomeTaskState.ready;
  final changes = <String, DeviceStatus Function(DeviceStatus)>{};

  void set(String id, {bool? active, required String display}) {
    changes[id] = (device) =>
        device.copyWith(active: active ?? device.active, displayValue: display);
  }

  switch (task.intent) {
    case 'prepare_home' when completed:
      set('LED-LIVING-01', active: true, display: 'ON');
      set('FAN-01', active: true, display: 'ON');
    case 'prepare_sleep' when completed:
      set('LED-LIVING-01', active: false, display: 'OFF');
      set('FAN-01', active: false, display: 'OFF');
      set('SERVO-WINDOW-01', active: false, display: '닫힘');
    case 'night_bed_exit' when started:
      set('LED-LIVING-01', active: true, display: 'ON · 15초');
    case 'quiet_mode_on' when started:
      for (final id in const ['LED-AIRCON-01']) {
        set(id, display: '깜빡임');
      }
    case 'quiet_mode_off' when completed:
      for (final id in const ['LED-AIRCON-01']) {
        changes[id] = (device) =>
            device.copyWith(displayValue: _switchDisplay(device.active));
      }
    case 'device_control' when completed:
      final id = task.controlDeviceId;
      final args = task.controlParameters;
      switch (task.controlAction) {
        case 'set_switch':
          final on = args['on'] == true;
          set(id, active: on, display: _switchDisplay(on));
        case 'set_position':
          final degrees = _number(args['position_degrees']);
          if (degrees != null) {
            final ref = positionRef(id, degrees);
            set(id, active: ref != 'closed', display: positionText(id, degrees));
          }
        case 'set_color':
          final color = args['color']?.toString() ?? '';
          final pattern = args['pattern']?.toString() ?? 'steady';
          if (color == 'off') {
            set(id, active: false, display: 'OFF');
          } else if (pattern == 'blink') {
            set(id, display: '깜빡임');
          } else {
            changes[id] = (device) =>
                device.copyWith(displayValue: _switchDisplay(device.active));
          }
        case 'set_level':
          final level = _number(args['level_percent']);
          if (level != null) {
            set(id, active: level > 0, display: '${level.round()}%');
          }
        case 'set_number':
          final value = _number(args['value']);
          if (value != null) set(id, display: '${value.toInt()}');
      }
  }
  if (changes.isEmpty) return devices;
  return devices
      .map((device) => changes[device.id]?.call(device) ?? device)
      .toList(growable: false);
}

/// Text for the LED `light_pattern` telemetry reading.
String lightPatternLabel(Object? raw) => switch (raw) {
      'off' => '꺼짐',
      'steady' => '켜짐',
      'blink' => '깜빡임',
      _ => '$raw',
    };
