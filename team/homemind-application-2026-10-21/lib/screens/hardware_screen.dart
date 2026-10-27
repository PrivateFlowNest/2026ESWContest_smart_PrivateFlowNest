import 'package:flutter/material.dart';

import '../core/theme/homemind_theme.dart';
import '../models/hardware_models.dart';
import '../state/app_controller.dart';
import '../widgets/common_widgets.dart';

class HardwareScreen extends StatelessWidget {
  const HardwareScreen({super.key, required this.controller});

  final AppController controller;

  @override
  Widget build(BuildContext context) {
    final controllable = controller.snapshot.devices
        .where((device) => device.controllable)
        .toList();
    final statusOnly = controller.snapshot.devices
        .where((device) => !device.controllable)
        .toList();

    return ScreenFrame(
      onRefresh: () => controller.refresh(),
      children: [
        BrandHeader(
          connected: controller.connected,
          usingDemoData: controller.usingDemoData,
        ),
        const SizedBox(height: 28),
        const SectionHeading(
          title: '하드웨어 제어',
          subtitle: 'Pi 4 백엔드에 등록된 ID만 표시하고 명령을 전송합니다.',
        ),
        if (controller.lastError != null) ...[
          const SizedBox(height: 16),
          _ErrorBanner(message: controller.lastError!),
        ],
        const SizedBox(height: 22),
        ...controllable.map(
          (device) => Padding(
            padding: const EdgeInsets.only(bottom: 14),
            child: _ControlCard(device: device, controller: controller),
          ),
        ),
        const SizedBox(height: 14),
        const SectionHeading(
          title: '상태 조회 장치',
          subtitle: '직접 제어 명령이 정의되지 않은 인식·모션·센서 장치입니다.',
        ),
        const SizedBox(height: 18),
        ...statusOnly.map(
          (device) => Padding(
            padding: const EdgeInsets.only(bottom: 12),
            child: _StatusCard(device: device),
          ),
        ),
      ],
    );
  }
}

class _ControlCard extends StatelessWidget {
  const _ControlCard({required this.device, required this.controller});

  final HardwareDevice device;
  final AppController controller;

  @override
  Widget build(BuildContext context) {
    final first = device.supportedCommands.first;
    final second = device.supportedCommands.last;
    return HomeMindCard(
      accent: _colorFor(device),
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          _DeviceHeader(device: device),
          const Divider(height: 28, color: HomeMindColors.border),
          Row(
            children: [
              PrimaryActionButton(
                label: _commandLabel(first),
                icon: _commandIcon(first),
                color: _colorFor(device),
                onPressed: controller.isLoading
                    ? null
                    : () => _send(context, first),
              ),
              const SizedBox(width: 10),
              OutlineActionButton(
                label: _commandLabel(second),
                icon: _commandIcon(second),
                color: _colorFor(device),
                onPressed: controller.isLoading
                    ? null
                    : () => _send(context, second),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Future<void> _send(BuildContext context, String command) async {
    final success = await controller.sendDeviceCommand(device, command);
    if (!context.mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(
          success
              ? controller.lastMessage ?? '${device.id} $command 요청을 보냈습니다.'
              : controller.lastError ?? '명령을 보내지 못했습니다.',
        ),
      ),
    );
  }
}

class _StatusCard extends StatelessWidget {
  const _StatusCard({required this.device});

  final HardwareDevice device;

  @override
  Widget build(BuildContext context) {
    return HomeMindCard(child: _DeviceHeader(device: device));
  }
}

class _DeviceHeader extends StatelessWidget {
  const _DeviceHeader({required this.device});

  final HardwareDevice device;

  @override
  Widget build(BuildContext context) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Icon(_iconFor(device), color: _colorFor(device), size: 32),
        const SizedBox(width: 13),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(device.name, style: Theme.of(context).textTheme.titleMedium),
              const SizedBox(height: 4),
              Text(
                device.id,
                style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                  color: HomeMindColors.mint,
                  fontWeight: FontWeight.w700,
                ),
              ),
              const SizedBox(height: 3),
              Text(
                '${device.controllerName}${device.alias == null ? '' : ' · ${device.alias}'}',
                style: Theme.of(context).textTheme.bodyMedium,
              ),
            ],
          ),
        ),
        const SizedBox(width: 8),
        StatusPill(
          label: device.pending
              ? '처리 중'
              : device.lastFailureCode != null
              ? '실행 안 됨'
              : _stateLabel(device.state),
          color: device.pending || device.lastFailureCode != null
              ? HomeMindColors.amber
              : device.online
              ? HomeMindColors.green
              : HomeMindColors.textSecondary,
        ),
      ],
    );
  }
}

class _ErrorBanner extends StatelessWidget {
  const _ErrorBanner({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: HomeMindColors.amber.withValues(alpha: 0.08),
        border: Border.all(color: HomeMindColors.amber.withValues(alpha: 0.45)),
        borderRadius: BorderRadius.circular(14),
      ),
      child: Row(
        children: [
          const Icon(Icons.link_off, color: HomeMindColors.amber),
          const SizedBox(width: 10),
          Expanded(child: Text(message)),
        ],
      ),
    );
  }
}

String _commandLabel(String command) => switch (command) {
  'OPEN' => '열기',
  'CLOSE' => '닫기',
  'ON' => '켜기',
  'OFF' => '끄기',
  _ => command,
};

IconData _commandIcon(String command) => switch (command) {
  'OPEN' => Icons.lock_open_outlined,
  'CLOSE' => Icons.lock_outline,
  'ON' => Icons.power_settings_new,
  'OFF' => Icons.power_off_outlined,
  _ => Icons.send_outlined,
};

String _stateLabel(String state) => switch (state) {
  'OPEN' => '열림',
  'CLOSED' => '닫힘',
  'ON' => '켜짐',
  'OFF' => '꺼짐',
  _ => '상태 대기',
};

IconData _iconFor(HardwareDevice device) {
  if (device.id.startsWith('SERVO-DOOR')) return Icons.door_front_door_outlined;
  if (device.id.startsWith('SERVO-WINDOW')) return Icons.window_outlined;
  if (device.id.startsWith('FAN')) return Icons.air;
  if (device.id.startsWith('LED')) return Icons.lightbulb_outline;
  if (device.id.startsWith('MIC')) return Icons.mic_none;
  if (device.id.startsWith('CAM')) return Icons.videocam_outlined;
  if (device.id.startsWith('MOTOR')) return Icons.open_with;
  return Icons.sensors_outlined;
}

Color _colorFor(HardwareDevice device) => switch (device.category) {
  'actuator' => HomeMindColors.mint,
  'indicator' => HomeMindColors.amber,
  'input' => HomeMindColors.blue,
  'motion' => HomeMindColors.purple,
  _ => HomeMindColors.green,
};
