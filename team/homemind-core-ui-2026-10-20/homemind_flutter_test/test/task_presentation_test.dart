import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/device_catalog.dart';
import 'package:homemind_flutter_test/src/launch_config.dart';
import 'package:homemind_flutter_test/src/models.dart';
import 'package:homemind_flutter_test/src/task_presentation.dart';

Map<String, dynamic> control(String device, String action,
        Map<String, dynamic> args, {String origin = 'voice_parser'}) =>
    {
      'target_device_id': device,
      'action': action,
      'parameters': args,
      'origin': origin,
      'ai_device': null,
      'ai_action': null,
    };

HomeTask task(String intent, HomeTaskState state,
        {String origin = '',
        String source = '',
        DateTime? updatedAt,
        Map<String, dynamic> parameters = const {}}) =>
    HomeTask(
      id: '$intent-$origin-${updatedAt?.millisecondsSinceEpoch}',
      title: intent,
      detail: '',
      state: state,
      priority: 40,
      revision: 1,
      intent: intent,
      origin: origin,
      sourceNodeId: source,
      parameters: parameters,
      updatedAt: updatedAt,
    );

void main() {
  test('device_control titles and approval questions are Korean', () {
    TaskText text(String device, String action, Map<String, dynamic> args) =>
        describeTask('device_control', control(device, action, args));

    expect(text('LED-LIVING-01', 'set_switch', {'on': true}).title, '거실 조명 켜기');
    expect(text('LED-LIVING-01', 'set_switch', {'on': false}).title, '거실 조명 끄기');
    expect(text('SERVO-DOOR-01', 'set_position', {'position_degrees': 90}).title,
        '현관문 열기');
    expect(
        text('SERVO-DOOR-01', 'set_position', {'position_degrees': 90})
            .question,
        '현관문을 열까요?');
    expect(text('SERVO-WINDOW-01', 'set_position', {'position_degrees': 0}).title,
        '창문 닫기');
    expect(text('SERVO-WINDOW-01', 'set_position', {'position_degrees': 45}).title,
        '창문 45°로 움직이기');
    expect(
        text('LED-TV-01', 'set_color', {'color': 'red', 'pattern': 'blink'})
            .title,
        'TV 음소거(깜빡임)');
    expect(
        text('LED-TV-01', 'set_color', {'color': 'red', 'pattern': 'blink'})
            .question,
        'TV를 음소거할까요?');
    expect(
        text('LED-TV-01', 'set_color', {'color': 'red', 'pattern': 'steady'})
            .title,
        'TV 음소거 해제');
    expect(text('LED-AIRCON-01', 'set_switch', {'on': true}).question,
        '에어컨을 켤까요?');
    expect(text('FAN-01', 'set_switch', {'on': true}).title, '환기 팬 켜기');
    expect(text('LED-LIVING-01', 'set_switch', {'on': true}).detail,
        contains('LED-LIVING-01'));
  });

  test('scenario intents use Korean names', () {
    expect(describeTask('prepare_home', const {}).title, '귀가 준비');
    expect(describeTask('prepare_sleep', const {}).title, '취침 준비');
    expect(describeTask('night_bed_exit', const {}).title, '야간 이동 조명');
    expect(describeTask('quiet_mode_on', const {}).title, '조용 모드 켜기');
    expect(describeTask('quiet_mode_off', const {}).title, '조용 모드 해제');
    expect(describeTask('prepare_sleep', const {}).question, '취침 준비를 시작할까요?');
  });

  test('object particle follows the last syllable', () {
    expect(objectParticle('에어컨'), '을');
    expect(objectParticle('창문'), '을');
    expect(objectParticle('거실 조명'), '을');
    expect(objectParticle('TV'), '를');
    expect(objectParticle('환기 팬'), '을');
    expect(objectParticle('세탁기'), '를');
  });

  test('every core rejection code has a Korean reason', () {
    for (final code in const [
      'DEVICE_NOT_WIRED',
      'CAPABILITY_NOT_ALLOWED',
      'SOURCE_NOT_ALLOWED',
      'POSITION_NOT_REGISTERED',
      'COLOR_NOT_SUPPORTED',
      'PARAMETERS_INVALID',
      'DEVICE_UNKNOWN',
      'ACK_TIMEOUT',
      'PROPOSAL_EXPIRED',
    ]) {
      final failure =
          TaskFailure(code: code, message: 'raw', occurredAt: DateTime(2026));
      expect(failureShortLabel(code), isNot(code), reason: code);
      expect(failureExplanation(failure), isNot('raw'), reason: code);
    }
    expect(failureShortLabel('DEVICE_NOT_WIRED'), '미배선 기기');
  });

  test('origin badge follows parameters.origin and source node', () {
    expect(task('device_control', HomeTaskState.completed,
            origin: 'voice_parser', source: 'NODE-RPI5-02')
        .originKind
        .label, '음성 명령');
    expect(task('device_control', HomeTaskState.proposed,
            origin: 'llm', source: 'NODE-RPI5-01')
        .isAiProposal, isTrue);
    expect(task('device_control', HomeTaskState.completed,
            origin: 'ui', source: 'NODE-RPI4-01')
        .originKind
        .label, '휴대폰');
    expect(task('device_control', HomeTaskState.completed,
            origin: 'ui', source: 'NODE-RPI5-01')
        .originKind
        .label, '화면');
    expect(task('quiet_mode_on', HomeTaskState.completed,
            origin: 'context_rule', source: 'NODE-RPI4-01')
        .originKind
        .label, '상황 감지');
  });

  test('quiet mode follows the newest quiet task or perception', () {
    final base = HomeSnapshot(
      connected: true,
      zone: HomeZone.desk,
      contextText: '',
      tasks: const [],
      devices: const [],
      safetyAlert: false,
      lastUpdated: DateTime(2026),
      systemStatus: HomeSystemStatus(
          overallStatus: 'healthy',
          storageUsedPercent: 0,
          warningThresholdPercent: 70,
          storageWarning: false,
          databaseBytes: 0,
          generatedAt: DateTime(2026),
          components: const []),
    );
    final on = task('quiet_mode_on', HomeTaskState.completed,
        origin: 'context_rule', updatedAt: DateTime(2026, 1, 1, 10));
    final off = task('quiet_mode_off', HomeTaskState.completed,
        origin: 'context_rule', updatedAt: DateTime(2026, 1, 1, 11));
    final rejectedOff = task('quiet_mode_off', HomeTaskState.rejected,
        updatedAt: DateTime(2026, 1, 1, 12));

    expect(base.quietMode.active, isFalse);
    expect(base.copyWith(tasks: [on]).quietMode.active, isTrue);
    expect(base.copyWith(tasks: [on, off]).quietMode.active, isFalse);
    expect(base.copyWith(tasks: [rejectedOff, on]).quietMode.active, isTrue);
    expect(
        base
            .copyWith(
                activity: ActivityContext(
                    activity: 'MEETING',
                    audioContext: 'ZOOM_SPEAKER',
                    quietRequired: true,
                    transition: 'ACTIVATED',
                    zone: 'desk',
                    confidence: .84,
                    observedAt: DateTime(2026)))
            .quietMode
            .byPerception,
        isTrue);
  });

  test('task effects show the LED quiet overlay and servo positions', () {
    final devices = buildOfficialDeviceStatuses(online: true);
    String display(List<DeviceStatus> list, String id) =>
        list.firstWhere((device) => device.id == id).displayValue;

    final quiet = applyTaskEffects(
        devices, task('quiet_mode_on', HomeTaskState.completed));
    expect(display(quiet, 'LED-AIRCON-01'), '깜빡임');

    final door = applyTaskEffects(
        devices,
        task('device_control', HomeTaskState.completed,
            parameters: control(
                'SERVO-DOOR-01', 'set_position', {'position_degrees': 90})));
    expect(display(door, 'SERVO-DOOR-01'), '열림');
  });

  test('device catalog follows registry v4 wiring', () {
    final listed = buildOfficialDeviceStatuses(online: true);
    DeviceStatus device(String id) =>
        listed.firstWhere((device) => device.id == id);
    expect(device('FAN-01').controllerNodeId, 'CTRL-ESP8266-LED-01');
    expect(device('FAN-01').pin, 'D2');
    expect(device('LED-AIRCON-01').pin, 'D1');
    expect(device('MIC-INDOOR-01').controllerNodeId, 'NODE-RPI5-02');
    expect(device('CAM-INDOOR-01').controllerNodeId, 'NODE-RPI5-02');
    expect(device('LED-LIVING-01').pin, 'D0');
    expect(device('SERVO-DOOR-01').hardware, '열기/닫기 모터');
    expect(device('SERVO-DOOR-01').pin, 'GPIO19');
    expect(device('SERVO-WINDOW-01').pin, 'GPIO21');
    expect(device('SERVO-WINDOW-01').displayValue, '닫힘');
    // The removed bathroom door and the doll axis keep their IDs but are not listed.
    final ids = listed.map((device) => device.id).toSet();
    for (final hidden in const [
      'SERVO-DOOR-02',
      'LED-WASHER-01',
      'LED-TV-01',
      'LED-LAPTOP-01',
      'LED-GUIDE-01',
      'SERVO-BED-CURTAIN-01',
      'FND-WASHER-01',
      'FND-AIRCON-01',
      'FND-TV-01',
      'MOTOR-XY-X-01',
      'MOTOR-XY-Y-01',
      'SENSOR-HOME-X-01',
      'SENSOR-HOME-Y-01'
    ]) {
      expect(ids, isNot(contains(hidden)));
      expect(officialDeviceById(hidden), isNotNull);
    }
  });

  test('launch config reads broker URL, mode and kiosk flags', () {
    final rehearsal = HomeMindLaunchConfig.from(
        webSocketUrl: 'ws://127.0.0.1:9001', kiosk: true);
    expect(rehearsal.host, '127.0.0.1');
    expect(rehearsal.webSocketPort, 9001);
    expect(rehearsal.kiosk, isTrue);
    expect(rehearsal.mockData, isFalse);

    final fromUrl = HomeMindLaunchConfig.from(
        query: {'broker': 'ws://10.10.10.2:9001/mqtt', 'kiosk': '0'},
        webSocketUrl: 'ws://127.0.0.1:9001',
        kiosk: true);
    expect(fromUrl.host, '10.10.10.2');
    expect(fromUrl.webSocketPath, '/mqtt');
    expect(fromUrl.kiosk, isFalse);
    expect(fromUrl.webSocketUrl, 'ws://10.10.10.2:9001/mqtt');

    final mock = HomeMindLaunchConfig.from(query: {'mode': 'mock', 'seed': '1'});
    expect(mock.mockData, isTrue);
    expect(mock.seedDemoData, isTrue);

    // Credentials in the URL and non-ws schemes are ignored.
    final rejected = HomeMindLaunchConfig.from(
        query: {'broker': 'ws://user:pw@10.0.0.9:9001'});
    expect(rejected.host, '10.10.10.2');
    expect(HomeMindLaunchConfig.from(query: {'broker': 'http://x:1'}).host,
        '10.10.10.2');
  });
}
