import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_application/core/api/homemind_api.dart';
import 'package:homemind_application/models/hardware_models.dart';
import 'package:homemind_application/state/app_controller.dart';

/// Mobile API 0.3 stand-in: commands become core tasks; state changes only
/// after the task completes.
class _FakeApi extends HomeMindApi {
  _FakeApi(this.taskStates);

  final List<String> taskStates;
  String confirmedState = 'OFF';
  int dashboardCalls = 0;

  HardwareDevice _fan({String? pending}) => HardwareDevice(
    id: 'FAN-01',
    name: '환기팬',
    hardware: 'FAN',
    controllerName: 'ESP32',
    category: 'actuator',
    online: true,
    state: confirmedState,
    supportedCommands: const ['ON', 'OFF'],
    pendingCommand: pending,
  );

  @override
  Future<HardwareSnapshot> getDashboard() async {
    dashboardCalls++;
    return HardwareSnapshot(
      connected: true,
      devices: [_fan()],
      updatedAt: 'now',
    );
  }

  @override
  Future<DeviceCommandResult> sendDeviceCommand(
    String deviceId,
    String command,
  ) async => DeviceCommandResult(
    device: _fan(pending: command),
    message: '요청을 core에 보냈습니다.',
    taskId: 'task-1',
    taskState: 'requested',
  );

  @override
  Future<TaskStatus> getTask(String taskId) async {
    final state = taskStates.isEmpty ? 'running' : taskStates.removeAt(0);
    if (state == 'completed') confirmedState = 'ON';
    return TaskStatus(
      state: state,
      failureCode: state == 'rejected' ? 'DEVICE_NOT_WIRED' : null,
    );
  }
}

Future<void> _settle(AppController controller) async {
  for (var i = 0; i < 40; i++) {
    await Future<void>.delayed(const Duration(milliseconds: 100));
  }
}

void main() {
  test('state waits for core completion (no optimistic change)', () async {
    final api = _FakeApi(['running', 'completed']);
    final controller = AppController(api: api);
    await controller.refresh();
    final fan = controller.snapshot.devices.single;
    expect(await controller.sendDeviceCommand(fan, 'ON'), isTrue);
    final pending = controller.snapshot.devices.single;
    expect(pending.state, 'OFF');
    expect(pending.pending, isTrue);
    await _settle(controller);
    final done = controller.snapshot.devices.single;
    expect(done.state, 'ON');
    expect(done.pending, isFalse);
    expect(controller.lastMessage, '장치가 응답했습니다.');
  });

  test('rejected task keeps the old state and explains why', () async {
    final api = _FakeApi(['rejected']);
    final controller = AppController(api: api);
    await controller.refresh();
    await controller.sendDeviceCommand(controller.snapshot.devices.single, 'ON');
    await _settle(controller);
    expect(controller.snapshot.devices.single.state, 'OFF');
    expect(controller.lastMessage, contains('DEVICE_NOT_WIRED'));
  });

  test('entrance door proposal asks for the 7-inch UI approval', () async {
    final api = _FakeApi(['proposed', 'proposed']);
    final controller = AppController(api: api);
    await controller.refresh();
    await controller.sendDeviceCommand(controller.snapshot.devices.single, 'ON');
    await Future<void>.delayed(const Duration(milliseconds: 700));
    expect(controller.lastMessage, '7인치 화면에서 승인하면 실행됩니다.');
  });
}
