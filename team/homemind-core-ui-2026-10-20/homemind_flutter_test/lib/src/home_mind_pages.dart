part of 'home_mind_app.dart';

class _HomePage extends StatelessWidget {
  const _HomePage(
      {required this.snapshot,
      required this.dataSource,
      this.dismissedLlmEventId,
      this.onDismissLlm});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;
  final String? dismissedLlmEventId;
  final ValueChanged<String>? onDismissLlm;

  @override
  Widget build(BuildContext context) {
    // AI proposals first, then other tasks waiting for approval.
    final pending = [
      ...snapshot.tasks.where((task) => task.isAiProposal),
      ...snapshot.tasks.where((task) =>
          task.state == HomeTaskState.proposed && !task.isAiProposal),
    ];
    final llm = snapshot.llmResult;
    final showLlm = llm != null &&
        llm.message != null &&
        llm.eventId != dismissedLlmEventId;
    final active = snapshot.tasks
        .where((task) => task.state == HomeTaskState.running)
        .toList();
    final highlighted = pending.isNotEmpty
        ? pending.first
        : active.isNotEmpty
            ? active.first
            : snapshot.tasks.isEmpty
                ? null
                : snapshot.tasks.first;
    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 14, 18, 12),
      child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
        Expanded(
          flex: 11,
          child: Column(children: [
            Expanded(
                child:
                    _ContextCard(snapshot: snapshot, dataSource: dataSource)),
            if (showLlm) ...[
              const SizedBox(height: 10),
              _AiMessageBar(
                  result: llm,
                  onDismiss: () => onDismissLlm?.call(llm.eventId)),
            ],
            const SizedBox(height: 12),
            SizedBox(
              height: 92,
              child: Row(children: [
                Expanded(
                  child: _MiniStateCard(
                      icon: Icons.place_outlined,
                      label: '현재 위치',
                      value: snapshot.zone.label,
                      color: _purple),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: _MiniStateCard(
                      icon: Icons.task_alt,
                      label: '진행 중',
                      value: '${active.length}개',
                      color: _mint),
                ),
                const SizedBox(width: 10),
                Expanded(
                  child: _MiniStateCard(
                    icon: Icons.touch_app_outlined,
                    label: '확인 필요',
                    value: '${pending.length}개',
                    color: pending.isEmpty ? Colors.white54 : _amber,
                  ),
                ),
              ]),
            ),
          ]),
        ),
        const SizedBox(width: 14),
        Expanded(
          flex: 9,
          child: _Panel(
            title: pending.isEmpty
                ? '최근 Task'
                : pending.first.isAiProposal
                    ? 'AI 제안'
                    : '확인이 필요해요',
            subtitle: pending.isEmpty
                ? 'HomeMind가 최근에 판단한 작업입니다.'
                : '실행할지 골라 주세요. 고르기 전에는 움직이지 않아요.',
            child: highlighted == null
                ? const _EmptyState(
                    icon: Icons.auto_awesome_outlined,
                    title: '지금은 할 일이 없어요',
                    detail: '새로운 상황을 감지하면 여기에 알려드릴게요.')
                : pending.isNotEmpty
                    ? _ProposalCard(
                        key: ValueKey('proposal-${highlighted.id}'),
                        task: highlighted,
                        dataSource: dataSource,
                        timeValid: snapshot.systemStatus.timeValid,
                        morePending: pending.length - 1)
                    : _CompactTaskCard(
                        task: highlighted,
                        dataSource: dataSource,
                        timeValid: snapshot.systemStatus.timeValid),
          ),
        ),
      ]),
    );
  }
}

class _ContextCard extends StatelessWidget {
  const _ContextCard({required this.snapshot, required this.dataSource});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;

  @override
  Widget build(BuildContext context) => Material(
        color: Colors.transparent,
        child: InkWell(
          borderRadius: BorderRadius.circular(22),
          onTap: () => _openPage(context,
              _ContextDetailPage(snapshot: snapshot, dataSource: dataSource)),
          child: Ink(
            padding: const EdgeInsets.all(22),
            decoration: BoxDecoration(
              gradient: const LinearGradient(
                  colors: [Color(0xFF253552), Color(0xFF392E5A)]),
              borderRadius: BorderRadius.circular(22),
            ),
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Row(children: [
                  const Icon(Icons.auto_awesome, color: _purple, size: 30),
                  const SizedBox(width: 10),
                  const Expanded(
                    child: Text('지금 HomeMind가 이해한 상황',
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                            color: Colors.white70,
                            fontSize: 17,
                            fontWeight: FontWeight.w700)),
                  ),
                  const SizedBox(width: 8),
                  _StatusPill(
                    icon: Icons.analytics_outlined,
                    label: snapshot.connected ? '신뢰도 높음' : '마지막 판단',
                    color: _purple,
                  ),
                ]),
                const Spacer(),
                Flexible(
                  flex: 6,
                  child: Text(snapshot.contextText,
                      style: const TextStyle(
                          fontSize: 27,
                          height: 1.25,
                          fontWeight: FontWeight.w900),
                      maxLines: 3,
                      overflow: TextOverflow.ellipsis),
                ),
                const Spacer(),
                Row(children: [
                  Icon(
                      snapshot.activity == null
                          ? Icons.sensors
                          : Icons.person_outline,
                      color: snapshot.activity == null
                          ? Colors.white54
                          : _purple,
                      size: 18),
                  const SizedBox(width: 7),
                  Expanded(
                    child: Text(
                        snapshot.activity == null
                            ? '${snapshot.sensorReadings.length}개 센서와 최근 행동을 바탕으로 판단했어요'
                            : '${snapshot.activity!.activityLabel} · '
                                '${snapshot.activity!.audioLabel} · '
                                '신뢰도 ${(snapshot.activity!.confidence * 100).round()}%',
                        maxLines: 1,
                        overflow: TextOverflow.ellipsis,
                        style: TextStyle(
                            color: snapshot.activity == null
                                ? Colors.white60
                                : Colors.white,
                            fontWeight: snapshot.activity == null
                                ? FontWeight.w400
                                : FontWeight.w800)),
                  ),
                  const Text('근거 보기',
                      style:
                          TextStyle(color: _mint, fontWeight: FontWeight.w800)),
                  const Icon(Icons.chevron_right, color: _mint),
                ]),
              ],
            ),
          ),
        ),
      );
}

enum _TaskFilter { all, decision, active, deferred, completed }

class _TaskPage extends StatefulWidget {
  const _TaskPage({required this.snapshot, required this.dataSource});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;

  @override
  State<_TaskPage> createState() => _TaskPageState();
}

class _TaskPageState extends State<_TaskPage> {
  _TaskFilter _filter = _TaskFilter.all;

  @override
  Widget build(BuildContext context) {
    final tasks = widget.snapshot.tasks.where((task) {
      return switch (_filter) {
        _TaskFilter.all => true,
        _TaskFilter.decision => task.state == HomeTaskState.proposed,
        _TaskFilter.active => task.state == HomeTaskState.running ||
            task.state == HomeTaskState.ready,
        _TaskFilter.deferred => task.state == HomeTaskState.deferred,
        _TaskFilter.completed => task.state == HomeTaskState.completed,
      };
    }).toList();
    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 14, 18, 12),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          const Text('Task',
              style: TextStyle(fontSize: 24, fontWeight: FontWeight.w900)),
          const SizedBox(width: 12),
          const Text('집이 스스로 처리하거나 확인을 기다리는 일',
              style: TextStyle(color: Colors.white54)),
          const Spacer(),
          ..._TaskFilter.values.map((filter) => Padding(
                padding: const EdgeInsets.only(left: 6),
                child: ChoiceChip(
                  label: Text(_filterLabel(filter)),
                  selected: _filter == filter,
                  onSelected: (_) => setState(() => _filter = filter),
                ),
              )),
        ]),
        const SizedBox(height: 12),
        Expanded(
          child: tasks.isEmpty
              ? const Card(
                  child: _EmptyState(
                      icon: Icons.task_alt,
                      title: '해당하는 Task가 없어요',
                      detail: '다른 분류를 선택하거나 새 상황을 기다려 주세요.'))
              : ListView.separated(
                  itemCount: tasks.length,
                  separatorBuilder: (_, __) => const SizedBox(height: 9),
                  itemBuilder: (_, index) => _TaskRow(
                    task: tasks[index],
                    dataSource: widget.dataSource,
                    timeValid: widget.snapshot.systemStatus.timeValid,
                  ),
                ),
        ),
      ]),
    );
  }
}

class _TaskRow extends StatelessWidget {
  const _TaskRow(
      {required this.task, required this.dataSource, required this.timeValid});
  final HomeTask task;
  final HomeMindDataSource dataSource;
  final bool timeValid;

  @override
  Widget build(BuildContext context) {
    final color = _taskStateColor(task.state);
    return Card(
      margin: EdgeInsets.zero,
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: () => _openPage(
            context,
            _TaskDetailPage(
                task: task, dataSource: dataSource, timeValid: timeValid)),
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
          child: Row(children: [
            CircleAvatar(
                backgroundColor: color.withValues(alpha: .14),
                child: Icon(_taskStateIcon(task.state), color: color)),
            const SizedBox(width: 13),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(children: [
                    Flexible(
                      child: Text(task.title,
                          style: const TextStyle(
                              fontSize: 17, fontWeight: FontWeight.w900),
                          overflow: TextOverflow.ellipsis),
                    ),
                    const SizedBox(width: 8),
                    _OriginBadge(origin: task.originKind),
                  ]),
                  Text(task.detail,
                      style: const TextStyle(color: Colors.white60),
                      maxLines: 1,
                      overflow: TextOverflow.ellipsis),
                ],
              ),
            ),
            if (task.failure != null)
              Padding(
                padding: const EdgeInsets.only(right: 12),
                child: Text(failureShortLabel(task.failure!.code),
                    style: const TextStyle(
                        color: _amber, fontWeight: FontWeight.w800)),
              ),
            _StatusPill(
                icon: _taskStateIcon(task.state),
                label: task.state.label,
                color: color),
            const Icon(Icons.chevron_right),
          ]),
        ),
      ),
    );
  }
}

class _DevicePage extends StatelessWidget {
  const _DevicePage({required this.snapshot});
  final HomeSnapshot snapshot;

  @override
  Widget build(BuildContext context) {
    // Offline wired devices first, then wired, then 미배선/미정 at the end.
    int rank(DeviceStatus device) =>
        !device.wired ? 2 : (device.online ? 1 : 0);
    final indexed = snapshot.devices.indexed.toList()
      ..sort((a, b) {
        final byRank = rank(a.$2).compareTo(rank(b.$2));
        return byRank != 0 ? byRank : a.$1.compareTo(b.$1);
      });
    final devices = indexed.map((entry) => entry.$2).toList();
    final unwired = devices.where((device) => !device.wired).length;
    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 14, 18, 12),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        Row(children: [
          const Text('기기',
              style: TextStyle(fontSize: 24, fontWeight: FontWeight.w900)),
          const SizedBox(width: 12),
          Text(
              '${devices.length}개 등록 · 미배선·미정 $unwired개 · 문제가 있는 기기를 먼저 표시해요',
              style: const TextStyle(color: Colors.white54)),
          const Spacer(),
          const _StatusPill(
              icon: Icons.timer_outlined,
              label: '15초 무응답 시 오프라인',
              color: _amber),
        ]),
        const SizedBox(height: 10),
        Expanded(
          child: GridView.builder(
            gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 3,
                mainAxisExtent: 132,
                crossAxisSpacing: 10,
                mainAxisSpacing: 10),
            itemCount: devices.length,
            itemBuilder: (_, index) => _DeviceCard(device: devices[index]),
          ),
        ),
      ]),
    );
  }
}

class _DeviceCard extends StatelessWidget {
  const _DeviceCard({required this.device});
  final DeviceStatus device;

  @override
  Widget build(BuildContext context) {
    final color = !device.wired
        ? Colors.white38
        : device.online
            ? (device.active ? _mint : Colors.white54)
            : _amber;
    final status = switch (device.wiringStatus) {
      WiringStatus.notWired => '미배선 · 명령은 거절돼요',
      WiringStatus.toConfirm => '배선 미정',
      WiringStatus.wired => device.online ? '온라인' : '오프라인 · 15초 초과',
    };
    return Card(
      margin: EdgeInsets.zero,
      child: InkWell(
        borderRadius: BorderRadius.circular(12),
        onTap: () => _deviceDetails(context, device, color),
        child: Padding(
          padding: const EdgeInsets.all(14),
          child: Row(children: [
            Icon(
                !device.wired
                    ? Icons.power_off_outlined
                    : device.online
                        ? Icons.memory
                        : Icons.portable_wifi_off,
                color: color,
                size: 31),
            const SizedBox(width: 11),
            Expanded(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(device.name,
                      style: const TextStyle(
                          fontSize: 16, fontWeight: FontWeight.w900),
                      overflow: TextOverflow.ellipsis),
                  Text('${device.location} · ${device.category}',
                      style:
                          const TextStyle(color: Colors.white54, fontSize: 12)),
                  Text(status, style: TextStyle(color: color, fontSize: 12)),
                  Text(device.id,
                      style: const TextStyle(color: _purple, fontSize: 11),
                      overflow: TextOverflow.ellipsis),
                ],
              ),
            ),
            Container(
              padding: const EdgeInsets.symmetric(horizontal: 9, vertical: 7),
              decoration: BoxDecoration(
                  color: Colors.black26,
                  borderRadius: BorderRadius.circular(8)),
              child: Text(
                  device.wired ? device.displayValue : device.wiringStatus.label,
                  style: TextStyle(color: color, fontWeight: FontWeight.w900)),
            ),
          ]),
        ),
      ),
    );
  }
}

class _PrivacyPage extends StatefulWidget {
  const _PrivacyPage({required this.snapshot, required this.dataSource});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;

  @override
  State<_PrivacyPage> createState() => _PrivacyPageState();
}

class _PrivacyPageState extends State<_PrivacyPage> {
  bool _saving = false;

  Future<void> _set(bool camera, bool microphone) async {
    if (_saving || !widget.snapshot.connected) return;
    setState(() => _saving = true);
    final mode = switch ((camera, microphone)) {
      (true, true) => PrivacyMode.normal,
      (false, true) => PrivacyMode.cameraBlocked,
      (true, false) => PrivacyMode.audioBlocked,
      (false, false) => PrivacyMode.private,
    };
    await widget.dataSource.setPrivacyMode(mode);
    await Future<void>.delayed(const Duration(milliseconds: 500));
    if (mounted) setState(() => _saving = false);
  }

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.fromLTRB(18, 14, 18, 12),
        child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
          Expanded(
            flex: 5,
            child: Column(children: [
              Card(
                margin: EdgeInsets.zero,
                child: Padding(
                  padding: const EdgeInsets.all(18),
                  child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Row(children: [
                          const Icon(Icons.shield_outlined,
                              color: _mint, size: 31),
                          const SizedBox(width: 10),
                          const Text('현재 개인정보 모드',
                              style: TextStyle(
                                  fontSize: 21, fontWeight: FontWeight.w900)),
                          const Spacer(),
                          _StatusPill(
                              icon: Icons.home_outlined,
                              label: widget.snapshot.privacy.mode.label,
                              color: _mint),
                        ]),
                        const SizedBox(height: 12),
                        const Text('카메라 영상은 Pi5에서 바로 판단하고 저장하지 않아요.',
                            style: TextStyle(fontSize: 17, height: 1.4)),
                        const Text('센서 기록과 Task 결과는 Pi4 SQLite에 보관합니다.',
                            style:
                                TextStyle(color: Colors.white60, height: 1.4)),
                      ]),
                ),
              ),
              const SizedBox(height: 10),
              Expanded(
                child: Card(
                  margin: EdgeInsets.zero,
                  child: ListView(padding: const EdgeInsets.all(5), children: [
                    if (_saving) const LinearProgressIndicator(minHeight: 3),
                    SwitchListTile(
                        secondary:
                            const Icon(Icons.videocam_outlined, color: _purple),
                        title: const Text('카메라 인식',
                            style: TextStyle(fontWeight: FontWeight.w800)),
                        subtitle: const Text('실시간 판단만 사용 · 영상 저장 안 함'),
                        value: widget.snapshot.privacy.cameraAllowed,
                        onChanged: _saving || !widget.snapshot.connected
                            ? null
                            : (value) => _set(value,
                                widget.snapshot.privacy.microphoneAllowed)),
                    SwitchListTile(
                        secondary: const Icon(Icons.mic_none, color: _purple),
                        title: const Text('마이크 소음 수치',
                            style: TextStyle(fontWeight: FontWeight.w800)),
                        subtitle: const Text('음성 녹음 안 함 · dB 수치만 사용'),
                        value: widget.snapshot.privacy.microphoneAllowed,
                        onChanged: _saving || !widget.snapshot.connected
                            ? null
                            : (value) => _set(
                                widget.snapshot.privacy.cameraAllowed, value)),
                    const ListTile(
                        leading: Icon(Icons.sensors, color: _purple),
                        title: Text('환경 센서',
                            style: TextStyle(fontWeight: FontWeight.w800)),
                        subtitle: Text('로컬 안전 규칙에 필요 · Pi4 내부에서만 처리'),
                        trailing:
                            Icon(Icons.lock_outline, color: Colors.white54)),
                  ]),
                ),
              ),
            ]),
          ),
          const SizedBox(width: 14),
          Expanded(
            flex: 4,
            child: Column(children: [
              Expanded(
                child: _InfoSection(
                    icon: widget.snapshot.privacy.cameraAllowed
                        ? Icons.videocam
                        : Icons.videocam_off,
                    title: '카메라 상태',
                    lines: [
                      widget.snapshot.privacy.cameraAllowed
                          ? '인식 허용됨'
                          : '사용자가 차단함',
                      widget.snapshot.privacy.recording
                          ? '녹화 중'
                          : '녹화 및 영상 보관 안 함',
                      '작동할 때 물리 LED로 표시'
                    ],
                    accent:
                        widget.snapshot.privacy.cameraAllowed ? _mint : _amber),
              ),
              const SizedBox(height: 10),
              const Expanded(
                child: _InfoSection(
                    icon: Icons.storage_outlined,
                    title: '데이터 보관',
                    lines: [
                      '저장 위치: Pi4 로컬 SQLite',
                      '자동 삭제: 사용 안 함',
                      '저장공간 70%에서 경고'
                    ],
                    accent: _purple),
              ),
            ]),
          ),
        ]),
      );
}

class _MorePage extends StatelessWidget {
  const _MorePage(
      {required this.snapshot, required this.dataSource, this.cameraPreview});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;
  final CameraPreviewClient? cameraPreview;

  @override
  Widget build(BuildContext context) {
    final items = <({IconData icon, String title, String detail, Widget page})>[
      (
        icon: Icons.qr_code_2,
        title: '휴대폰 연결',
        detail: '일회용 QR과 확인 코드',
        page: _PairingPage(dataSource: dataSource)
      ),
      (
        icon: Icons.phonelink_lock_outlined,
        title: '연결된 휴대폰',
        detail: '목록 확인과 연결 해제',
        page: _PhonesPage(dataSource: dataSource)
      ),
      (
        icon: Icons.videocam_outlined,
        title: '카메라',
        detail: '실시간 영상과 인식 상태',
        page: _CameraPage(dataSource: dataSource, preview: cameraPreview)
      ),
      (
        icon: Icons.history,
        title: '기록',
        detail: 'SQLite에 보관된 Task',
        page: _HistoryPage(snapshot: snapshot)
      ),
      (
        icon: Icons.monitor_heart_outlined,
        title: '시스템 상태',
        detail: 'Core·MQTT·DB·저장공간',
        page: _SystemPage(snapshot: snapshot, dataSource: dataSource)
      ),
      (
        icon: Icons.rate_review_outlined,
        title: '피드백',
        detail: '상황 판단과 동작 평가',
        page: _FeedbackPage(dataSource: dataSource)
      ),
      (
        icon: Icons.science_outlined,
        title: '시나리오 시험',
        detail: '개발 중에만 사용하는 도구',
        page: _DemoToolsPage(snapshot: snapshot, dataSource: dataSource)
      ),
    ];
    return Padding(
      padding: const EdgeInsets.fromLTRB(18, 14, 18, 12),
      child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
        const Text('더보기',
            style: TextStyle(fontSize: 24, fontWeight: FontWeight.w900)),
        const Text('설정, 기록, 연결 및 진단 기능',
            style: TextStyle(color: Colors.white54)),
        const SizedBox(height: 12),
        Expanded(
          child: GridView.builder(
            gridDelegate: const SliverGridDelegateWithFixedCrossAxisCount(
                crossAxisCount: 4,
                mainAxisExtent: 145,
                crossAxisSpacing: 12,
                mainAxisSpacing: 12),
            itemCount: items.length,
            itemBuilder: (_, index) {
              final item = items[index];
              return Card(
                margin: EdgeInsets.zero,
                child: InkWell(
                  borderRadius: BorderRadius.circular(12),
                  onTap: () => _openPage(context, item.page),
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Column(
                        crossAxisAlignment: CrossAxisAlignment.start,
                        children: [
                          Row(children: [
                            Icon(item.icon, color: _mint, size: 30),
                            const Spacer(),
                            const Icon(Icons.chevron_right,
                                color: Colors.white38),
                          ]),
                          const Spacer(),
                          Text(item.title,
                              style: const TextStyle(
                                  fontSize: 18, fontWeight: FontWeight.w900)),
                          Text(item.detail,
                              style: const TextStyle(
                                  color: Colors.white54, fontSize: 13)),
                        ]),
                  ),
                ),
              );
            },
          ),
        ),
      ]),
    );
  }
}

String _filterLabel(_TaskFilter filter) => switch (filter) {
      _TaskFilter.all => '전체',
      _TaskFilter.decision => '확인 필요',
      _TaskFilter.active => '실행 중',
      _TaskFilter.deferred => '보류',
      _TaskFilter.completed => '완료',
    };

void _deviceDetails(BuildContext context, DeviceStatus device, Color color) {
  final age = device.lastSeenAt == null
      ? null
      : DateTime.now().difference(device.lastSeenAt!).inSeconds.clamp(0, 9999);
  showModalBottomSheet<void>(
    context: context,
    showDragHandle: true,
    isScrollControlled: true,
    builder: (_) => SingleChildScrollView(
      padding: const EdgeInsets.fromLTRB(24, 0, 24, 24),
      child: Column(mainAxisSize: MainAxisSize.min, children: [
        ListTile(
          leading: Icon(Icons.memory, color: color, size: 34),
          title: Text(device.name,
              style:
                  const TextStyle(fontSize: 22, fontWeight: FontWeight.w900)),
          subtitle: Text('${device.location} · ${device.category}'),
          trailing: _StatusPill(
              icon: !device.wired
                  ? Icons.power_off_outlined
                  : device.online
                      ? Icons.check
                      : Icons.link_off,
              label: !device.wired
                  ? device.wiringStatus.label
                  : device.online
                      ? '온라인'
                      : '오프라인',
              color: color),
        ),
        _KeyValue(label: '공식 장치 ID', value: device.id),
        _KeyValue(label: '담당 제어기', value: device.controllerNodeId),
        _KeyValue(
            label: '하드웨어 · 핀',
            value: [device.hardware, device.pin]
                .where((value) => value.isNotEmpty)
                .join(' · ')),
        _KeyValue(
            label: '배선 상태',
            value: switch (device.wiringStatus) {
              WiringStatus.wired => '배선됨 (전장 확정안)',
              WiringStatus.notWired => '미배선 · core가 명령을 거절해요',
              WiringStatus.toConfirm => '미정 · 담당 제어기 배선 확인 전',
            }),
        _KeyValue(label: '마지막 신호', value: age == null ? '기록 없음' : '$age초 전'),
        const SizedBox(height: 12),
        SizedBox(
          width: double.infinity,
          child: OutlinedButton.icon(
              onPressed: () => Navigator.pop(context),
              icon: const Icon(Icons.health_and_safety_outlined),
              label: const Text('연결 진단')),
        ),
      ]),
    ),
  );
}
