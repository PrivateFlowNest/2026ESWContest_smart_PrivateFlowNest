import 'package:flutter/material.dart';

import '../core/theme/homemind_theme.dart';
import '../state/app_controller.dart';
import 'approvals_screen.dart';
import 'hardware_screen.dart';
import 'settings_screen.dart';

class AppShell extends StatelessWidget {
  const AppShell({super.key, required this.controller});

  final AppController controller;

  @override
  Widget build(BuildContext context) {
    return AnimatedBuilder(
      animation: controller,
      builder: (context, _) {
        final pages = [
          HardwareScreen(controller: controller),
          ApprovalsScreen(controller: controller),
          SettingsScreen(controller: controller),
        ];
        final waiting = controller.approvals.length;
        return Scaffold(
          body: Stack(
            children: [
              IndexedStack(index: controller.selectedIndex, children: pages),
              if (controller.isLoading)
                const Positioned(
                  left: 0,
                  right: 0,
                  top: 0,
                  child: LinearProgressIndicator(
                    minHeight: 2,
                    color: HomeMindColors.mint,
                    backgroundColor: Colors.transparent,
                  ),
                ),
              if (controller.approvalAlert != null)
                Positioned(
                  left: 0,
                  right: 0,
                  top: 0,
                  child: ApprovalAlertBanner(controller: controller),
                ),
            ],
          ),
          bottomNavigationBar: NavigationBar(
            selectedIndex: controller.selectedIndex,
            onDestinationSelected: controller.selectTab,
            destinations: [
              const NavigationDestination(
                icon: Icon(Icons.home_outlined),
                selectedIcon: Icon(Icons.home),
                label: '장치',
              ),
              NavigationDestination(
                icon: Badge(
                  isLabelVisible: waiting > 0,
                  label: Text('$waiting'),
                  child: const Icon(Icons.fact_check_outlined),
                ),
                selectedIcon: Badge(
                  isLabelVisible: waiting > 0,
                  label: Text('$waiting'),
                  child: const Icon(Icons.fact_check),
                ),
                label: '승인',
              ),
              const NavigationDestination(
                icon: Icon(Icons.settings_outlined),
                selectedIcon: Icon(Icons.settings),
                label: '설정',
              ),
            ],
          ),
        );
      },
    );
  }
}
