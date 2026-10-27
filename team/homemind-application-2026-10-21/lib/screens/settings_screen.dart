import 'package:flutter/material.dart';

import '../core/theme/homemind_theme.dart';
import '../state/app_controller.dart';
import '../widgets/common_widgets.dart';
import 'pairing_screen.dart';

class SettingsScreen extends StatelessWidget {
  const SettingsScreen({super.key, required this.controller});

  final AppController controller;

  @override
  Widget build(BuildContext context) {
    return ScreenFrame(
      children: [
        BrandHeader(
          connected: controller.connected,
          usingDemoData: controller.usingDemoData,
        ),
        const SizedBox(height: 28),
        const SectionHeading(title: '설정', subtitle: '연결과 개인정보 보호 상태를 관리하세요.'),
        const SizedBox(height: 22),
        HomeMindCard(
          accent: controller.connected
              ? HomeMindColors.green
              : HomeMindColors.amber,
          child: Column(
            children: [
              _SettingRow(
                icon: Icons.hub_outlined,
                title: 'HomeMind Core',
                subtitle: controller.baseUrl,
                trailing: StatusPill(
                  label: controller.connected ? '연결됨' : '데모 모드',
                  color: controller.connected
                      ? HomeMindColors.green
                      : HomeMindColors.amber,
                ),
              ),
              const Divider(height: 30, color: HomeMindColors.border),
              _SettingRow(
                icon: Icons.key_outlined,
                title: '페어링',
                subtitle: controller.credential == null
                    ? '연결된 기기 없음'
                    : '${controller.credential!.deviceName} · 토큰 ${controller.credential!.tokenId}',
                trailing: StatusPill(
                  label: controller.paired ? '토큰 저장됨' : '미연결',
                  color: controller.paired
                      ? HomeMindColors.green
                      : HomeMindColors.amber,
                ),
              ),
              const Divider(height: 30, color: HomeMindColors.border),
              Wrap(
                spacing: 10,
                runSpacing: 10,
                children: [
                  PrimaryActionButton(
                    label: '기기 연결 설정',
                    icon: Icons.qr_code_scanner,
                    expanded: false,
                    onPressed: () => Navigator.of(context).push(
                      MaterialPageRoute<void>(
                        builder: (_) => PairingScreen(controller: controller),
                      ),
                    ),
                  ),
                  if (controller.paired)
                    OutlinedButton.icon(
                      onPressed: () => _confirmForget(context),
                      icon: const Icon(Icons.link_off),
                      label: const Text('연결 해제'),
                    ),
                ],
              ),
            ],
          ),
        ),
        const SizedBox(height: 16),
        const HomeMindCard(
          child: Column(
            children: [
              _SettingRow(
                icon: Icons.account_tree_outlined,
                title: 'Pi 4 ID 매핑',
                subtitle: 'GET /api/v1/hardware-map',
                trailing: StatusPill(label: 'API', color: HomeMindColors.green),
              ),
              Divider(height: 30, color: HomeMindColors.border),
              _SettingRow(
                icon: Icons.swap_horiz,
                title: '장치 명령',
                subtitle: 'POST /api/v1/devices/{id}/commands',
                trailing: StatusPill(label: 'MQTT', color: HomeMindColors.mint),
              ),
            ],
          ),
        ),
        const SizedBox(height: 16),
        HomeMindCard(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text('베타 빌드 정보', style: Theme.of(context).textTheme.titleMedium),
              const SizedBox(height: 10),
              Text(
                'Flutter UI · Pi 4 ID 기반 FastAPI 연결점',
                style: Theme.of(context).textTheme.bodyMedium,
              ),
              Text(
                'QR 페어링 토큰(Authorization: Bearer)으로 인증',
                style: Theme.of(context).textTheme.bodyMedium,
              ),
            ],
          ),
        ),
      ],
    );
  }

  Future<void> _confirmForget(BuildContext context) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('이 휴대폰의 연결을 해제할까요?'),
        content: const Text(
          '저장된 토큰을 지웁니다. 다시 쓰려면 기기에서 새 QR을 만들어 스캔하세요. '
          '기기 쪽 토큰은 homemind-mobile-api --revoke-token으로 폐기합니다.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('취소'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('연결 해제'),
          ),
        ],
      ),
    );
    if (confirmed == true) await controller.forgetPairing();
  }
}

class _SettingRow extends StatelessWidget {
  const _SettingRow({
    required this.icon,
    required this.title,
    required this.subtitle,
    required this.trailing,
  });

  final IconData icon;
  final String title;
  final String subtitle;
  final Widget trailing;

  @override
  Widget build(BuildContext context) {
    return Row(
      children: [
        Icon(icon, color: HomeMindColors.mint, size: 29),
        const SizedBox(width: 13),
        Expanded(
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(title, style: Theme.of(context).textTheme.titleMedium),
              const SizedBox(height: 4),
              Text(subtitle, style: Theme.of(context).textTheme.bodyMedium),
            ],
          ),
        ),
        const SizedBox(width: 8),
        trailing,
      ],
    );
  }
}
