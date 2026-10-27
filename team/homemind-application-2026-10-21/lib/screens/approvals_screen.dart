import 'package:flutter/material.dart';

import '../core/theme/homemind_theme.dart';
import '../models/approval_models.dart';
import '../state/app_controller.dart';
import '../widgets/common_widgets.dart';

/// Requests core holds for approval: AI proposals, home scenarios and the
/// entrance door. The phone approves or rejects them; opening the entrance
/// door is approved only on the 7-inch screen in the house.
class ApprovalsScreen extends StatelessWidget {
  const ApprovalsScreen({super.key, required this.controller});

  final AppController controller;

  @override
  Widget build(BuildContext context) {
    final items = controller.approvals;
    return ScreenFrame(
      onRefresh: controller.refreshApprovals,
      children: [
        BrandHeader(
          connected: controller.connected,
          usingDemoData: controller.usingDemoData,
        ),
        const SizedBox(height: 28),
        SectionHeading(
          title: '승인 대기',
          subtitle: controller.liveConnected
              ? '새 요청이 오면 바로 알려 드려요.'
              : '앱이 켜져 있는 동안 15초마다 확인해요.',
        ),
        const SizedBox(height: 22),
        if (items.isEmpty)
          const HomeMindCard(
            child: Row(
              children: [
                Icon(Icons.task_alt, color: HomeMindColors.green, size: 30),
                SizedBox(width: 13),
                Expanded(child: Text('기다리는 요청이 없어요.')),
              ],
            ),
          ),
        ...items.map(
          (item) => Padding(
            padding: const EdgeInsets.only(bottom: 14),
            child: _ApprovalCard(item: item, controller: controller),
          ),
        ),
      ],
    );
  }
}

class _ApprovalCard extends StatelessWidget {
  const _ApprovalCard({required this.item, required this.controller});

  final ApprovalItem item;
  final AppController controller;

  @override
  Widget build(BuildContext context) {
    final color = item.phoneCanApprove
        ? HomeMindColors.mint
        : HomeMindColors.amber;
    return HomeMindCard(
      accent: color,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Icon(_iconFor(item), color: color, size: 32),
              const SizedBox(width: 13),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      item.title,
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    const SizedBox(height: 4),
                    Text(
                      _waiting(item),
                      style: Theme.of(context).textTheme.bodyMedium,
                    ),
                  ],
                ),
              ),
              const SizedBox(width: 8),
              StatusPill(label: item.originLabel, color: color),
            ],
          ),
          if (item.note != null) ...[
            const SizedBox(height: 12),
            Row(
              children: [
                const Icon(
                  Icons.home_outlined,
                  color: HomeMindColors.amber,
                  size: 18,
                ),
                const SizedBox(width: 8),
                Expanded(
                  child: Text(
                    item.note!,
                    style: Theme.of(context).textTheme.bodyMedium?.copyWith(
                      color: HomeMindColors.amber,
                    ),
                  ),
                ),
              ],
            ),
          ],
          const Divider(height: 28, color: HomeMindColors.border),
          Row(
            children: [
              PrimaryActionButton(
                label: '승인',
                icon: Icons.check,
                color: HomeMindColors.mint,
                onPressed: controller.isLoading || !item.phoneCanApprove
                    ? null
                    : () => _decide(context, approve: true),
              ),
              const SizedBox(width: 10),
              OutlineActionButton(
                label: '거절',
                icon: Icons.close,
                onPressed: controller.isLoading
                    ? null
                    : () => _decide(context, approve: false),
              ),
            ],
          ),
        ],
      ),
    );
  }

  Future<void> _decide(BuildContext context, {required bool approve}) async {
    final success = await controller.decideApproval(item, approve: approve);
    if (!context.mounted) return;
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(
          success
              ? controller.lastMessage ?? '보냈습니다.'
              : controller.lastError ?? '보내지 못했습니다.',
        ),
      ),
    );
  }
}

/// Banner over every tab when a new request arrives while the app runs.
class ApprovalAlertBanner extends StatelessWidget {
  const ApprovalAlertBanner({super.key, required this.controller});

  final AppController controller;

  @override
  Widget build(BuildContext context) {
    return SafeArea(
      child: Padding(
        padding: const EdgeInsets.fromLTRB(12, 8, 12, 0),
        child: Material(
          color: HomeMindColors.surfaceRaised,
          shape: RoundedRectangleBorder(
            borderRadius: BorderRadius.circular(14),
            side: const BorderSide(color: HomeMindColors.mint),
          ),
          child: InkWell(
            borderRadius: BorderRadius.circular(14),
            onTap: () => controller.selectTab(AppController.approvalsTab),
            child: Padding(
              padding: const EdgeInsets.fromLTRB(14, 10, 4, 10),
              child: Row(
                children: [
                  const Icon(
                    Icons.notifications_active_outlined,
                    color: HomeMindColors.mint,
                  ),
                  const SizedBox(width: 10),
                  Expanded(
                    child: Text(
                      controller.approvalAlert ?? '',
                      style: const TextStyle(fontWeight: FontWeight.w700),
                    ),
                  ),
                  TextButton(
                    onPressed: () =>
                        controller.selectTab(AppController.approvalsTab),
                    child: const Text('보기'),
                  ),
                  IconButton(
                    tooltip: '닫기',
                    onPressed: controller.dismissApprovalAlert,
                    icon: const Icon(Icons.close, size: 18),
                  ),
                ],
              ),
            ),
          ),
        ),
      ),
    );
  }
}

String _waiting(ApprovalItem item) {
  if (item.waitingSinceMs <= 0) return '방금 요청';
  final minutes = DateTime.now()
      .difference(DateTime.fromMillisecondsSinceEpoch(item.waitingSinceMs))
      .inMinutes;
  return minutes < 1 ? '방금 요청' : '$minutes분째 기다리는 중';
}

IconData _iconFor(ApprovalItem item) {
  if (item.targetDevice.startsWith('SERVO-DOOR')) {
    return Icons.door_front_door_outlined;
  }
  if (item.origin == 'llm') return Icons.auto_awesome_outlined;
  if (item.intent != 'device_control') return Icons.home_work_outlined;
  return Icons.lightbulb_outline;
}
