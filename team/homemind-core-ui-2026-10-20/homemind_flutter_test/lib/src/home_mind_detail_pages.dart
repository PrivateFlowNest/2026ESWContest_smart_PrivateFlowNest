part of 'home_mind_app.dart';

class _TaskDetailPage extends StatefulWidget {
  const _TaskDetailPage(
      {required this.task, required this.dataSource, required this.timeValid});
  final HomeTask task;
  final HomeMindDataSource dataSource;
  final bool timeValid;

  @override
  State<_TaskDetailPage> createState() => _TaskDetailPageState();
}

class _TaskDetailPageState extends State<_TaskDetailPage> {
  bool _processing = false;

  Future<void> _act(TaskAction action) async {
    if (_processing) return;
    setState(() => _processing = true);
    await widget.dataSource.actOnTask(widget.task.id, action);
    if (mounted) Navigator.pop(context);
  }

  @override
  Widget build(BuildContext context) {
    final task = widget.task;
    final stages = ['감지', '제안', '준비', '확인', '실행', '완료'];
    final progress = switch (task.state) {
      HomeTaskState.proposed => 3,
      HomeTaskState.ready => 2,
      HomeTaskState.running => 4,
      HomeTaskState.completed => 5,
      _ => 1,
    };
    return _DetailScaffold(
      title: 'Task 상세',
      child: ListView(padding: const EdgeInsets.all(20), children: [
        Row(children: [
          Expanded(
            child: Text(task.title,
                style:
                    const TextStyle(fontSize: 28, fontWeight: FontWeight.w900)),
          ),
          _StatusPill(
              icon: _taskStateIcon(task.state),
              label: task.state.label,
              color: _taskStateColor(task.state)),
        ]),
        const SizedBox(height: 8),
        Row(children: [
          _OriginBadge(origin: task.originKind),
          const SizedBox(width: 10),
          Expanded(
            child: Text(task.detail,
                style: const TextStyle(color: Colors.white70, fontSize: 18)),
          ),
        ]),
        if (task.state == HomeTaskState.proposed) ...[
          const SizedBox(height: 10),
          Text(taskQuestion(task),
              style: TextStyle(
                  color: task.originKind == TaskOrigin.ai ? _purple : _amber,
                  fontSize: 22,
                  fontWeight: FontWeight.w900)),
        ],
        const SizedBox(height: 18),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(18),
            child: Row(
              children: List.generate(stages.length, (index) {
                final done = index <= progress;
                return Expanded(
                  child: Row(children: [
                    Column(children: [
                      CircleAvatar(
                        radius: 16,
                        backgroundColor:
                            done ? _mint : Colors.white.withValues(alpha: .08),
                        child: Icon(done ? Icons.check : Icons.more_horiz,
                            size: 18, color: done ? _navy : Colors.white38),
                      ),
                      const SizedBox(height: 5),
                      Text(stages[index],
                          style: TextStyle(
                              color: done ? Colors.white : Colors.white38,
                              fontWeight: FontWeight.w700)),
                    ]),
                    if (index != stages.length - 1)
                      Expanded(
                          child: Divider(
                              color: done ? _mint : Colors.white12,
                              thickness: 2)),
                  ]),
                );
              }),
            ),
          ),
        ),
        const SizedBox(height: 12),
        Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Expanded(
            child: _InfoSection(
                icon: Icons.psychology_outlined,
                title: '판단 근거',
                lines: [
                  task.confidence > 0
                      ? '인식 신뢰도 ${(task.confidence * 100).round()}%'
                      : '인식 신뢰도 수신 대기',
                  '요청 출처 ${task.originKind.label}'
                      '${task.sourceNodeId.isEmpty ? '' : ' · ${task.sourceNodeId}'}',
                  '요청 의도 ${task.intent.isEmpty ? '확인 중' : task.intent}'
                      '${task.isDeviceControl && task.controlAction.isNotEmpty ? ' · ${task.controlAction}' : ''}',
                ]),
          ),
          const SizedBox(width: 12),
          Expanded(
            child: _InfoSection(icon: Icons.swap_horiz, title: '예상 변화', lines: [
              '대상 ${_targetLabel(task)}',
              '실행 결과를 ACK로 확인',
              'revision ${task.revision} · Pi4 기록'
            ]),
          ),
        ]),
        if (task.failure != null) ...[
          const SizedBox(height: 12),
          _FailureBox(
              failure: task.failure!,
              time: widget.timeValid
                  ? _clock(task.failure!.occurredAt)
                  : '시간 미확인'),
        ],
        if (task.state == HomeTaskState.proposed) ...[
          const SizedBox(height: 16),
          Row(children: [
            Expanded(
              child: FilledButton.icon(
                  onPressed:
                      _processing ? null : () => _act(TaskAction.approve),
                  icon: _processing
                      ? const SizedBox.square(
                          dimension: 18,
                          child: CircularProgressIndicator(strokeWidth: 2))
                      : const Icon(Icons.check),
                  label: Text(_processing ? '처리 중…' : '실행')),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: OutlinedButton.icon(
                  onPressed: _processing ? null : () => _act(TaskAction.reject),
                  icon: const Icon(Icons.close),
                  label: const Text('취소 (실행하지 않음)')),
            ),
          ]),
        ] else ...[
          const SizedBox(height: 16),
          OutlinedButton.icon(
            onPressed: () => _openPage(
              context,
              _FeedbackPage(
                dataSource: widget.dataSource,
                initialType: FeedbackType.action,
                taskId: task.id,
              ),
            ),
            icon: const Icon(Icons.rate_review_outlined),
            label: const Text('이 Task의 결과 피드백'),
          ),
        ],
      ]),
    );
  }
}

String _targetLabel(HomeTask task) {
  final id = task.isDeviceControl ? task.controlDeviceId : task.targetDevice;
  if (id.isEmpty) return '확인 중';
  final device = officialDeviceById(id);
  if (device != null) return '${device.name} ($id)';
  return id == 'HOME-ENV-01' ? '집 전체 시나리오' : id;
}

class _ContextDetailPage extends StatelessWidget {
  const _ContextDetailPage({required this.snapshot, required this.dataSource});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;

  @override
  Widget build(BuildContext context) => _DetailScaffold(
        title: '상황 상세',
        child: ListView(padding: const EdgeInsets.all(20), children: [
          Card(
            color: const Color(0xFF292541),
            child: Padding(
              padding: const EdgeInsets.all(22),
              child: Row(children: [
                const Icon(Icons.auto_awesome, color: _purple, size: 40),
                const SizedBox(width: 16),
                Expanded(
                  child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        const Text('현재 Context',
                            style: TextStyle(color: Colors.white54)),
                        Text(snapshot.contextText,
                            style: const TextStyle(
                                fontSize: 25, fontWeight: FontWeight.w900)),
                      ]),
                ),
                const _StatusPill(
                    icon: Icons.analytics_outlined,
                    label: '신뢰도 높음',
                    color: _purple),
              ]),
            ),
          ),
          const SizedBox(height: 12),
          const Text('이렇게 판단했어요',
              style: TextStyle(fontSize: 21, fontWeight: FontWeight.w900)),
          const SizedBox(height: 6),
          if (snapshot.activity != null)
            Card(
              child: ListTile(
                leading: Icon(
                    snapshot.activity!.quietRequired
                        ? Icons.volume_off_outlined
                        : Icons.person_outline,
                    color: _purple),
                title: Text(
                    '${snapshot.activity!.activityLabel} · ${snapshot.activity!.audioLabel}',
                    style: const TextStyle(fontWeight: FontWeight.w800)),
                subtitle: Text(
                    'Pi5 상황 인식 · ${snapshot.activity!.zone} · ${_clock(snapshot.activity!.observedAt)}'
                    '${snapshot.activity!.quietRequired ? ' · 조용히 해야 해요' : ''}'),
                trailing: Text(
                    '${(snapshot.activity!.confidence * 100).round()}%',
                    style: const TextStyle(
                        color: _purple,
                        fontSize: 20,
                        fontWeight: FontWeight.w900)),
              ),
            ),
          ...snapshot.sensorReadings.take(4).map((reading) => Card(
                child: ListTile(
                  leading: const Icon(Icons.sensors, color: _mint),
                  title: Text(reading.label,
                      style: const TextStyle(fontWeight: FontWeight.w800)),
                  subtitle: Text(
                      '${reading.sourceDeviceId} · ${_clock(reading.updatedAt)}'),
                  trailing: Text('${reading.value}${reading.unit}',
                      style: const TextStyle(
                          color: _mint,
                          fontSize: 20,
                          fontWeight: FontWeight.w900)),
                ),
              )),
          Card(
            child: ListTile(
              leading: const Icon(Icons.schedule, color: _purple),
              title: const Text('시간과 최근 행동',
                  style: TextStyle(fontWeight: FontWeight.w800)),
              subtitle: Text(
                  '현재 위치 ${snapshot.zone.label} · 최근 갱신 ${_clock(snapshot.lastUpdated)}'),
            ),
          ),
          const SizedBox(height: 12),
          Row(children: [
            Expanded(
              child: OutlinedButton.icon(
                  onPressed: () async {
                    await dataSource.submitFeedback(
                        type: FeedbackType.context,
                        rating: FeedbackRating.positive,
                        note: 'Context detail quick feedback');
                    if (context.mounted) {
                      ScaffoldMessenger.of(context).showSnackBar(
                          const SnackBar(content: Text('상황 피드백을 Pi4로 보냈어요.')));
                    }
                  },
                  icon: const Icon(Icons.thumb_up_outlined),
                  label: const Text('상황이 맞아요')),
            ),
            const SizedBox(width: 10),
            Expanded(
              child: OutlinedButton.icon(
                  onPressed: () => _openPage(
                      context,
                      _FeedbackPage(
                          dataSource: dataSource,
                          initialType: FeedbackType.context)),
                  icon: const Icon(Icons.edit_outlined),
                  label: const Text('상황이 달라요')),
            ),
          ]),
        ]),
      );
}

class _HistoryPage extends StatelessWidget {
  const _HistoryPage({required this.snapshot});
  final HomeSnapshot snapshot;

  @override
  Widget build(BuildContext context) => _DetailScaffold(
        title: 'Task 기록',
        child: snapshot.tasks.isEmpty
            ? const _EmptyState(
                icon: Icons.history,
                title: '저장된 Task가 없어요',
                detail: 'Pi4 SQLite에 기록이 생기면 이곳에서 확인할 수 있어요.')
            : ListView.separated(
                padding: const EdgeInsets.all(18),
                itemCount: snapshot.tasks.length,
                separatorBuilder: (_, __) => const SizedBox(height: 8),
                itemBuilder: (_, index) {
                  final task = snapshot.tasks[index];
                  return Card(
                    child: ListTile(
                      leading: Icon(_taskStateIcon(task.state),
                          color: _taskStateColor(task.state)),
                      title: Text(task.title,
                          style: const TextStyle(fontWeight: FontWeight.w900)),
                      subtitle: Text(task.failure == null
                          ? '${task.originKind == TaskOrigin.unknown ? '' : '${task.originKind.label} · '}${task.detail}'
                          : '${failureShortLabel(task.failure!.code)} · ${failureExplanation(task.failure!)}'),
                      trailing: Text(task.state.label,
                          style: TextStyle(
                              color: _taskStateColor(task.state),
                              fontWeight: FontWeight.w800)),
                    ),
                  );
                }),
      );
}

class _SystemPage extends StatelessWidget {
  const _SystemPage({required this.snapshot, required this.dataSource});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;

  @override
  Widget build(BuildContext context) {
    final status = snapshot.systemStatus;
    return _DetailScaffold(
      title: '시스템 상태',
      actions: [
        IconButton(
            tooltip: '새로고침',
            onPressed: dataSource.requestSnapshot,
            icon: const Icon(Icons.refresh))
      ],
      child: ListView(padding: const EdgeInsets.all(18), children: [
        GridView.count(
          crossAxisCount: 4,
          shrinkWrap: true,
          physics: const NeverScrollableScrollPhysics(),
          childAspectRatio: 2,
          crossAxisSpacing: 9,
          mainAxisSpacing: 9,
          children: [
            _MetricCard(
                icon: Icons.hub,
                label: 'Pi4 Core',
                value: snapshot.connected ? '정상' : '오프라인',
                color: snapshot.connected ? _mint : _amber),
            _MetricCard(
                icon: Icons.storage_outlined,
                label: '저장공간',
                value: '${status.storageUsedPercent.toStringAsFixed(1)}%',
                color: status.storageWarning ? _amber : _mint),
            _MetricCard(
                icon: Icons.dataset_outlined,
                label: 'SQLite DB',
                value: _bytes(status.databaseBytes),
                color: _purple),
            const _MetricCard(
                icon: Icons.thermostat,
                label: 'Pi5 온도',
                value: '수집 대기',
                color: Colors.white54),
          ],
        ),
        const SizedBox(height: 10),
        if (!status.timeValid) ...[
          const _NoticeBox(
              icon: Icons.schedule_outlined,
              title: '시간을 아직 확인하지 못했어요',
              detail: '데이터는 보존하지만 날짜는 신뢰하지 않습니다.',
              color: _amber),
          const SizedBox(height: 10),
        ],
        Card(
          child: Padding(
            padding: const EdgeInsets.all(16),
            child: Column(children: [
              Row(children: [
                const Text('저장공간 사용률',
                    style: TextStyle(fontWeight: FontWeight.w900)),
                const Spacer(),
                Text(
                    status.storageWarning
                        ? '70% 경고 발생'
                        : '자동 삭제 안 함 · 70%에서 경고',
                    style: const TextStyle(color: Colors.white60)),
              ]),
              const SizedBox(height: 10),
              LinearProgressIndicator(
                  value: (status.storageUsedPercent / 100).clamp(0, 1),
                  minHeight: 12,
                  borderRadius: BorderRadius.circular(6),
                  color: status.storageWarning ? _amber : _mint),
            ]),
          ),
        ),
        const SizedBox(height: 10),
        ...status.components.map((component) {
          final ok =
              component.status == 'healthy' || component.status == 'online';
          return Card(
            child: ListTile(
              leading: Icon(ok ? Icons.check_circle : Icons.warning_amber,
                  color: ok ? _mint : _amber),
              title: Text(component.name,
                  style: const TextStyle(fontWeight: FontWeight.w800)),
              subtitle: Text(
                  '${component.id}${component.detail.isEmpty ? '' : ' · ${component.detail}'}'),
              trailing: Text(ok ? '정상' : '점검 필요',
                  style: TextStyle(
                      color: ok ? _mint : _amber, fontWeight: FontWeight.w800)),
            ),
          );
        }),
        const Card(
          child: ListTile(
            leading: Icon(Icons.speed, color: Colors.white54),
            title: Text('CPU · 메모리 · 지연시간',
                style: TextStyle(fontWeight: FontWeight.w800)),
            subtitle: Text('Pi5 시스템 측정 연결 전'),
            trailing: Text('수집 대기', style: TextStyle(color: Colors.white54)),
          ),
        ),
        const Card(
          child: ListTile(
            leading: Icon(Icons.info_outline, color: _purple),
            title: Text('버전', style: TextStyle(fontWeight: FontWeight.w800)),
            subtitle: Text('HomeMind UI prototype v8'),
            trailing:
                Text('업데이트 확인 전', style: TextStyle(color: Colors.white54)),
          ),
        ),
      ]),
    );
  }
}

/// Phone pairing: a one-time QR from the Mobile API on NODE-RPI4-01 (pfn/v1/mobile/pairing/*). The code stays on
/// this screen only and the page asks for a fresh one when it expires.
class _PairingPage extends StatefulWidget {
  const _PairingPage({required this.dataSource});
  final HomeMindDataSource dataSource;

  @override
  State<_PairingPage> createState() => _PairingPageState();
}

class _PairingPageState extends State<_PairingPage> {
  PhonePairingOffer? _offer;
  String? _error;
  bool _loading = false;
  Timer? _tick;

  @override
  void initState() {
    super.initState();
    _request();
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _tick?.cancel();
    super.dispose();
  }

  Future<void> _request() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final offer = await widget.dataSource.requestPhonePairing();
      if (!mounted) return;
      setState(() {
        _offer = offer;
        _loading = false;
      });
    } on PhonePairingException catch (error) {
      if (!mounted) return;
      setState(() {
        _offer = null;
        _error = error.message;
        _loading = false;
      });
    }
  }

  @override
  Widget build(BuildContext context) {
    final offer = _offer;
    final left =
        offer?.expiresAt.difference(DateTime.now());
    final expired = left != null && left <= Duration.zero;
    return _DetailScaffold(
      title: '휴대폰 연결',
      actions: [
        TextButton.icon(
          onPressed: _loading ? null : _request,
          icon: const Icon(Icons.refresh),
          label: const Text('새 QR'),
        ),
      ],
      child: Center(
        child: SizedBox(
          width: 900,
          child: Card(
            child: Padding(
              padding: const EdgeInsets.all(24),
              child: Row(children: [
                Container(
                  width: 330,
                  height: 330,
                  decoration: BoxDecoration(
                      color: Colors.white,
                      borderRadius: BorderRadius.circular(14)),
                  alignment: Alignment.center,
                  child: _loading
                      ? const CircularProgressIndicator(color: Colors.black54)
                      : offer == null || expired
                          ? Icon(
                              expired
                                  ? Icons.timer_off_outlined
                                  : Icons.qr_code_2,
                              color: Colors.black26,
                              size: 150)
                          : PairingQrCode(data: offer.qrPayload, size: 310),
                ),
                const SizedBox(width: 30),
                Expanded(child: _details(offer, left, expired)),
              ]),
            ),
          ),
        ),
      ),
    );
  }

  Widget _details(PhonePairingOffer? offer, Duration? left, bool expired) {
    if (_loading) {
      return const Text('Pi4에 일회용 QR을 요청하고 있어요…',
          style: TextStyle(fontSize: 22, fontWeight: FontWeight.w800));
    }
    if (offer == null) {
      return Column(
          mainAxisAlignment: MainAxisAlignment.center,
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            const _StatusPill(
                icon: Icons.error_outline, label: 'QR을 받지 못했어요', color: _amber),
            const SizedBox(height: 14),
            Text(_error ?? '',
                style: const TextStyle(
                    fontSize: 20, fontWeight: FontWeight.w800, height: 1.4)),
            const SizedBox(height: 18),
            FilledButton.icon(
                onPressed: _request,
                icon: const Icon(Icons.refresh),
                label: const Text('다시 시도')),
          ]);
    }
    final remaining = expired
        ? '만료됐어요'
        : '${left!.inMinutes.toString().padLeft(2, '0')}:${(left.inSeconds % 60).toString().padLeft(2, '0')} 남음';
    return Column(
        mainAxisAlignment: MainAxisAlignment.center,
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          if (offer.simulated)
            const Padding(
              padding: EdgeInsets.only(bottom: 12),
              child: _StatusPill(
                  icon: Icons.science_outlined,
                  label: '시험용 QR (시뮬레이션)',
                  color: _amber),
            ),
          const Text('HomeMind 앱 → 기기 연결 → QR 스캔',
              style: TextStyle(fontSize: 24, fontWeight: FontWeight.w900)),
          const SizedBox(height: 14),
          const Text('확인 코드', style: TextStyle(color: Colors.white60)),
          Text(offer.pairingCode,
              style: const TextStyle(
                  color: _mint,
                  fontSize: 40,
                  fontWeight: FontWeight.w900,
                  letterSpacing: 3)),
          const Text('앱에 보이는 코드와 같은지 확인하세요.',
              style: TextStyle(color: Colors.white60)),
          const SizedBox(height: 16),
          Text('${offer.deviceName} · ${offer.baseUrl}',
              style: const TextStyle(color: Colors.white70)),
          const SizedBox(height: 6),
          Row(children: [
            Icon(Icons.timer_outlined,
                color: expired ? _amber : Colors.white60, size: 20),
            const SizedBox(width: 6),
            Text(remaining,
                style: TextStyle(
                    color: expired ? _amber : Colors.white70,
                    fontWeight: FontWeight.w700)),
          ]),
          const SizedBox(height: 10),
          const Text('한 번만 쓸 수 있는 코드예요. 화면을 찍어 다른 사람에게 보내지 마세요.',
              style: TextStyle(color: Colors.white54, height: 1.4)),
          if (expired) ...[
            const SizedBox(height: 14),
            FilledButton.icon(
                onPressed: _request,
                icon: const Icon(Icons.refresh),
                label: const Text('새 QR 만들기')),
          ],
        ]);
  }
}

/// Paired phones (pfn/v1/mobile/phones/*): names, pairing time and last request from the Mobile API on NODE-RPI4-01,
/// and unpairing one. An unpaired phone is refused at once and has to scan a new QR. A phone that pairs while this
/// page is open appears without a refresh.
class _PhonesPage extends StatefulWidget {
  const _PhonesPage({required this.dataSource});
  final HomeMindDataSource dataSource;

  @override
  State<_PhonesPage> createState() => _PhonesPageState();
}

class _PhonesPageState extends State<_PhonesPage> {
  PairedPhones? _phones;
  String? _error;
  bool _loading = false;
  String? _busyId;
  StreamSubscription<PairedPhones>? _updates;

  @override
  void initState() {
    super.initState();
    _updates = widget.dataSource.pairedPhoneUpdates.listen((phones) {
      if (!mounted) return;
      setState(() {
        _phones = phones;
        _error = null;
      });
    });
    _load();
  }

  @override
  void dispose() {
    _updates?.cancel();
    super.dispose();
  }

  Future<void> _load() async {
    setState(() {
      _loading = true;
      _error = null;
    });
    try {
      final phones = await widget.dataSource.requestPairedPhones();
      if (!mounted) return;
      setState(() {
        _phones = phones;
        _loading = false;
      });
    } on PhonePairingException catch (error) {
      if (!mounted) return;
      setState(() {
        _error = error.message;
        _loading = false;
      });
    }
  }

  Future<void> _unpair(PairedPhone phone) async {
    final confirmed = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('${phone.deviceName} 연결 해제'),
        content: const Text(
            '이 휴대폰은 바로 HomeMind를 쓸 수 없게 돼요.\n다시 쓰려면 휴대폰 연결 화면의 새 QR로 연결해야 해요.',
            style: TextStyle(height: 1.4)),
        actions: [
          TextButton(
              onPressed: () => Navigator.of(context).pop(false),
              child: const Text('취소')),
          FilledButton(
              onPressed: () => Navigator.of(context).pop(true),
              child: const Text('연결 해제')),
        ],
      ),
    );
    if (confirmed != true || !mounted) return;
    final messenger = ScaffoldMessenger.of(context);
    setState(() => _busyId = phone.phoneId);
    try {
      final phones = await widget.dataSource.unpairPhone(phone.phoneId);
      if (!mounted) return;
      setState(() {
        _phones = phones;
        _busyId = null;
      });
      messenger.showSnackBar(SnackBar(
          content: Text(phones.revoked == false
              ? '${phone.deviceName}은(는) 이미 연결이 끊겨 있었어요.'
              : '${phone.deviceName}의 연결을 끊었어요.')));
    } on PhonePairingException catch (error) {
      if (!mounted) return;
      setState(() => _busyId = null);
      messenger.showSnackBar(SnackBar(content: Text(error.message)));
    }
  }

  @override
  Widget build(BuildContext context) => _DetailScaffold(
        title: '연결된 휴대폰',
        actions: [
          TextButton.icon(
            onPressed: _loading ? null : _load,
            icon: const Icon(Icons.refresh),
            label: const Text('새로고침'),
          ),
        ],
        child: Center(child: SizedBox(width: 900, child: _body())),
      );

  Widget _body() {
    final phones = _phones;
    if (phones == null) {
      if (_loading) {
        return const Center(
            child: Text('Pi4에 연결된 휴대폰 목록을 묻고 있어요…',
                style: TextStyle(fontSize: 22, fontWeight: FontWeight.w800)));
      }
      return Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          const _StatusPill(
              icon: Icons.error_outline, label: '목록을 받지 못했어요', color: _amber),
          const SizedBox(height: 14),
          Text(_error ?? '',
              style: const TextStyle(fontSize: 20, fontWeight: FontWeight.w800)),
          const SizedBox(height: 18),
          FilledButton.icon(
              onPressed: _load,
              icon: const Icon(Icons.refresh),
              label: const Text('다시 시도')),
        ]),
      );
    }
    return ListView(padding: const EdgeInsets.symmetric(vertical: 16), children: [
      if (phones.simulated)
        const Padding(
          padding: EdgeInsets.only(bottom: 10),
          child: Align(
            alignment: Alignment.centerLeft,
            child: _StatusPill(
                icon: Icons.science_outlined,
                label: '시험용 목록 (시뮬레이션)',
                color: _amber),
          ),
        ),
      Text(
          phones.phones.isEmpty
              ? '연결된 휴대폰이 없어요. 더보기 → 휴대폰 연결의 QR로 연결하세요.'
              : '${phones.phones.length}대 연결됨 · 연결을 끊으면 그 휴대폰은 바로 쓸 수 없어요.',
          style: const TextStyle(color: Colors.white70, fontSize: 16)),
      const SizedBox(height: 10),
      for (final phone in phones.phones)
        Card(
          child: ListTile(
            leading: const Icon(Icons.smartphone, color: _mint, size: 32),
            title: Text(phone.deviceName,
                style: const TextStyle(fontSize: 19, fontWeight: FontWeight.w900)),
            subtitle: Text(
                '연결 ${_phoneDateText(phone.pairedAt)} · 최근 접속 ${_phoneSeenText(phone.lastSeenAt)}',
                style: const TextStyle(color: Colors.white60)),
            trailing: _busyId == phone.phoneId
                ? const SizedBox(
                    width: 28, height: 28, child: CircularProgressIndicator(strokeWidth: 3))
                : OutlinedButton.icon(
                    onPressed: _busyId == null ? () => _unpair(phone) : null,
                    icon: const Icon(Icons.link_off),
                    label: const Text('연결 해제')),
          ),
        ),
    ]);
  }
}

String _phoneDateText(DateTime? at) => at == null
    ? '알 수 없음'
    : '${at.month}월 ${at.day}일 ${at.hour.toString().padLeft(2, '0')}:${at.minute.toString().padLeft(2, '0')}';

/// Last request since the Mobile API started ("기록 없음" also right after a Pi4 restart).
String _phoneSeenText(DateTime? at) {
  if (at == null) return '기록 없음';
  final ago = DateTime.now().difference(at);
  if (ago.inMinutes < 1) return '방금';
  if (ago.inHours < 1) return '${ago.inMinutes}분 전';
  if (ago.inDays < 1) return '${ago.inHours}시간 전';
  return _phoneDateText(at);
}

/// Live video of NODE-RPI5-02's cameras over the home router (camera_preview.dart: newest JPEG only, nothing
/// stored, never to an outside server) with each camera's recognition state. Asks nothing while the privacy mode
/// blocks cameras; the vision service refuses then as well.
class _CameraPage extends StatefulWidget {
  const _CameraPage({required this.dataSource, this.preview});
  final HomeMindDataSource dataSource;
  final CameraPreviewClient? preview;

  @override
  State<_CameraPage> createState() => _CameraPageState();
}

class _CameraPageState extends State<_CameraPage> {
  static const _framePeriod = Duration(milliseconds: 350);
  static const _statusPeriod = Duration(seconds: 3);

  bool _active = true;
  late bool _cameraAllowed;
  final Map<String, Uint8List> _frames = {};
  final Map<String, String> _errors = {};
  final Set<String> _inFlight = {};
  CameraPreviewStatus? _status;
  Timer? _frameTimer;
  Timer? _statusTimer;
  StreamSubscription<HomeSnapshot>? _snapshots;

  @override
  void initState() {
    super.initState();
    _cameraAllowed = widget.dataSource.current.privacy.cameraAllowed;
    _snapshots = widget.dataSource.snapshots.listen((snapshot) {
      if (!mounted || snapshot.privacy.cameraAllowed == _cameraAllowed) return;
      setState(() {
        _cameraAllowed = snapshot.privacy.cameraAllowed;
        if (!_cameraAllowed) _frames.clear();
      });
    });
    _start();
  }

  @override
  void dispose() {
    _frameTimer?.cancel();
    _statusTimer?.cancel();
    _snapshots?.cancel();
    super.dispose();
  }

  void _start() {
    _frameTimer?.cancel();
    _statusTimer?.cancel();
    if (widget.preview == null || !_active) return;
    _pollFrames();
    _pollStatus();
    _frameTimer = Timer.periodic(_framePeriod, (_) => _pollFrames());
    _statusTimer = Timer.periodic(_statusPeriod, (_) => _pollStatus());
  }

  void _toggle() {
    setState(() {
      _active = !_active;
      if (!_active) {
        _frames.clear();
        _errors.clear();
      }
    });
    _start();
  }

  void _pollFrames() {
    final preview = widget.preview;
    if (preview == null || !_active || !_cameraAllowed) return;
    for (final camera in kPreviewCameras) {
      if (!_inFlight.add(camera.id)) continue;  // the previous request of this camera is still open
      preview.frame(camera.id).then<void>((bytes) {
        if (!mounted || !_active || !_cameraAllowed) return;
        setState(() {
          _frames[camera.id] = bytes;
          _errors.remove(camera.id);
        });
      }, onError: (Object error) {
        if (!mounted) return;
        setState(() {
          _frames.remove(camera.id);
          _errors[camera.id] =
              error is CameraPreviewException ? error.message : '영상을 받지 못했어요.';
        });
      }).whenComplete(() {
        _inFlight.remove(camera.id);
      });
    }
  }

  void _pollStatus() {
    final preview = widget.preview;
    if (preview == null || !_cameraAllowed) return;
    preview.status().then<void>((status) {
      if (mounted) setState(() => _status = status);
    }, onError: (Object _) {});
  }

  @override
  Widget build(BuildContext context) => _DetailScaffold(
        title: '카메라',
        actions: [
          if (widget.preview != null)
            TextButton.icon(
              onPressed: _toggle,
              icon: Icon(_active ? Icons.pause : Icons.play_arrow),
              label: Text(_active ? '영상 멈춤' : '영상 보기'),
            ),
        ],
        child: Padding(
          padding: const EdgeInsets.fromLTRB(18, 12, 18, 14),
          child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            _banner(),
            const SizedBox(height: 10),
            Expanded(
              child: Row(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
                for (final (index, camera) in kPreviewCameras.indexed) ...[
                  if (index > 0) const SizedBox(width: 14),
                  Expanded(child: _tile(camera.id, camera.name)),
                ],
              ]),
            ),
          ]),
        ),
      );

  Widget _banner() {
    if (widget.preview == null) {
      return const Row(children: [
        _StatusPill(icon: Icons.videocam_off, label: '영상 연결 안 됨', color: _amber),
        SizedBox(width: 12),
        Expanded(
            child: Text('Pi5-01 ui.env에 HOMEMIND_CAMERA_PREVIEW_URL이 없어요. 노트북 데모에서는 실제 영상을 열지 않습니다.',
                style: TextStyle(color: Colors.white60))),
      ]);
    }
    if (!_cameraAllowed) {
      return const _StatusPill(icon: Icons.lock_outline, label: '사생활 보호 모드: 카메라 영상 꺼짐', color: _amber);
    }
    if (!_active) {
      return const _StatusPill(icon: Icons.pause_circle_outline, label: '영상 멈춤', color: Colors.white54);
    }
    return const _StatusPill(
        icon: Icons.fiber_manual_record, label: '카메라 사용 중 · 집 안 공유기로만 · 저장 안 함', color: _mint);
  }

  Widget _tile(String id, String name) {
    final bytes = _frames[id];
    final showing = bytes != null && _active && _cameraAllowed && widget.preview != null;
    final detecting = _status?.detecting[id];
    final message = !_cameraAllowed
        ? '사생활 보호 모드'
        : widget.preview == null || !_active
            ? '영상 꺼짐'
            : _errors[id] ?? '영상을 받는 중…';
    return Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
      Expanded(
        child: Container(
          width: double.infinity,
          clipBehavior: Clip.antiAlias,
          decoration: BoxDecoration(
              color: Colors.black,
              borderRadius: BorderRadius.circular(14),
              border: Border.all(color: Colors.white12)),
          child: showing
              ? Image.memory(bytes,
                  gaplessPlayback: true, fit: BoxFit.contain, semanticLabel: '$name 실시간 영상')
              : Center(
                  child: Padding(
                    padding: const EdgeInsets.all(16),
                    child: Text(message,
                        textAlign: TextAlign.center,
                        style: const TextStyle(color: Colors.white60, fontSize: 16)),
                  ),
                ),
        ),
      ),
      const SizedBox(height: 8),
      Text(name, style: const TextStyle(fontSize: 18, fontWeight: FontWeight.w900)),
      Text(
          detecting == null
              ? '인식 상태 확인 전'
              : detecting
                  ? '인식 중: 인형 위치 판단'
                  : '빈 장면 촬영 전: 영상만, 인식 안 함',
          style: TextStyle(color: detecting == true ? _mint : Colors.white60)),
    ]);
  }
}

class _FeedbackPage extends StatefulWidget {
  const _FeedbackPage({
    required this.dataSource,
    this.initialType = FeedbackType.context,
    this.taskId,
  });

  final HomeMindDataSource dataSource;
  final FeedbackType initialType;
  final String? taskId;

  @override
  State<_FeedbackPage> createState() => _FeedbackPageState();
}

class _FeedbackPageState extends State<_FeedbackPage> {
  late FeedbackType _type;
  String? _choice;
  bool _sending = false;
  final _note = TextEditingController();

  @override
  void initState() {
    super.initState();
    _type = widget.initialType;
  }

  @override
  void dispose() {
    _note.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => _DetailScaffold(
        title: '피드백',
        child: Center(
          child: SizedBox(
            width: 720,
            child: Card(
              child: ListView(padding: const EdgeInsets.all(24), children: [
                const Text('어떤 부분을 알려주실 건가요?',
                    style:
                        TextStyle(fontSize: 22, fontWeight: FontWeight.w900)),
                const SizedBox(height: 10),
                SegmentedButton<FeedbackType>(
                    segments: const [
                      ButtonSegment(
                          value: FeedbackType.context, label: Text('상황 판단')),
                      ButtonSegment(
                          value: FeedbackType.action, label: Text('기기 동작')),
                    ],
                    selected: {
                      _type
                    },
                    onSelectionChanged: _sending
                        ? null
                        : (value) => setState(() => _type = value.first)),
                const SizedBox(height: 16),
                Wrap(spacing: 10, runSpacing: 10, children: [
                  for (final choice in ['정확했어요', '조금 달라요', '도움이 안 됐어요'])
                    ChoiceChip(
                        label: Text(choice),
                        selected: _choice == choice,
                        onSelected: _sending
                            ? null
                            : (_) => setState(() => _choice = choice)),
                ]),
                const SizedBox(height: 14),
                TextField(
                    controller: _note,
                    maxLines: 3,
                    decoration: const InputDecoration(
                        labelText: '추가 설명 (선택)', border: OutlineInputBorder())),
                const SizedBox(height: 12),
                FilledButton.icon(
                    onPressed: _choice == null || _sending
                        ? null
                        : () async {
                            setState(() => _sending = true);
                            final rating = _choice == '정확했어요'
                                ? FeedbackRating.positive
                                : FeedbackRating.negative;
                            await widget.dataSource.submitFeedback(
                              type: _type,
                              rating: rating,
                              taskId: widget.taskId,
                              note: _note.text,
                            );
                            if (context.mounted) {
                              ScaffoldMessenger.of(context).showSnackBar(
                                  const SnackBar(
                                      content: Text('피드백을 Pi4에 저장했습니다.')));
                              Navigator.pop(context);
                            }
                          },
                    icon: _sending
                        ? const SizedBox.square(
                            dimension: 18,
                            child: CircularProgressIndicator(strokeWidth: 2))
                        : const Icon(Icons.send_outlined),
                    label: Text(_sending ? '저장 중…' : '피드백 보내기')),
                const SizedBox(height: 6),
                const Text('선택하지 않고 닫으면 응답 없음(UNKNOWN)으로 남습니다.',
                    textAlign: TextAlign.center,
                    style: TextStyle(color: Colors.white54, fontSize: 13)),
              ]),
            ),
          ),
        ),
      );
}

class _DemoToolsPage extends StatelessWidget {
  const _DemoToolsPage({required this.snapshot, required this.dataSource});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;

  @override
  Widget build(BuildContext context) {
    final buttons =
        <({IconData icon, String title, Future<void> Function() run})>[
      (
        icon: Icons.door_front_door_outlined,
        title: '1. 귀가 감지',
        run: dataSource.simulateArrival
      ),
      (
        icon: Icons.bedtime_outlined,
        title: '2. 취침 제안',
        run: dataSource.proposeSleep
      ),
      (
        icon: Icons.nightlight_outlined,
        title: '3. 야간 이탈',
        run: dataSource.simulateNightExit
      ),
      // A real P0 alert comes from sensors/core (NODE-RPI5-02 publishes system/safety/alert). The test button only
      // exists for the mock and the anonymous development broker; with a broker login (device mode) the ACL would
      // refuse it anyway.
      if (canTestSafetyAlert(dataSource))
        (
          icon: Icons.warning_amber_rounded,
          title: 'P0 경고',
          run: dataSource.triggerSafetyAlert
        ),
      if (!dataSource.isLive)
        (
          icon: Icons.timer_off_outlined,
          title: '명령 시간초과',
          run: dataSource.simulateCommandTimeout
        ),
      if (!dataSource.isLive)
        (
          icon: Icons.portable_wifi_off,
          title: '15초 오프라인',
          run: dataSource.simulateDeviceOffline
        ),
      (
        icon: snapshot.connected ? Icons.link_off : Icons.link,
        title: snapshot.connected ? '연결 끊기' : '다시 연결',
        run: () => dataSource.setConnected(!snapshot.connected)
      ),
      if (dataSource case final HomeMindDemoActions demo) ...[
        (
          icon: Icons.mic_none,
          title: '음성: 거실 조명 켜기',
          run: demo.demoVoiceCommand
        ),
        (
          icon: Icons.auto_awesome,
          title: 'AI 제안 (실행/취소)',
          run: demo.demoAiProposal
        ),
        (
          icon: Icons.record_voice_over,
          title: 'AI 되묻기',
          run: demo.demoAiClarify
        ),
        (
          icon: Icons.door_front_door_outlined,
          title: '음성: 현관문 열기 (승인)',
          run: demo.demoEntranceDoorRequest
        ),
        (
          icon: Icons.power_off_outlined,
          title: '미배선 기기 요청 (거절)',
          run: demo.demoUnwiredRequest
        ),
        (
          icon: Icons.smartphone,
          title: '휴대폰: 창문 닫기',
          run: demo.demoPhoneCommand
        ),
        (
          icon: Icons.record_voice_over_outlined,
          title: '말하는 중 → 준비 표시',
          run: demo.demoPartialHint
        ),
        (
          icon: Icons.volume_off_outlined,
          title: '회의 감지 → 조용 모드',
          run: demo.demoMeetingStarted
        ),
        (
          icon: Icons.volume_up_outlined,
          title: '회의 끝 → 조용 모드 해제',
          run: demo.demoMeetingEnded
        ),
      ],
    ];
    return _DetailScaffold(
      title: '시나리오 시험 도구',
      child: Padding(
        padding: const EdgeInsets.all(20),
        child: Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
          const _NoticeBox(
              icon: Icons.science_outlined,
              title: '개발 및 발표 연습용 화면',
              detail: '일반 사용자는 홈에서 이 버튼들을 보지 않습니다. 실제 연결 모드에서는 명령이 Pi4로 전송됩니다.',
              color: _amber),
          const SizedBox(height: 16),
          Expanded(
            child: GridView.count(
              crossAxisCount: 4,
              mainAxisSpacing: 10,
              crossAxisSpacing: 10,
              childAspectRatio: 3.3,
              children: buttons
                  .map((button) => FilledButton.tonalIcon(
                      onPressed: button.run,
                      icon: Icon(button.icon),
                      label: Text(button.title,
                          maxLines: 2,
                          textAlign: TextAlign.center,
                          style: const TextStyle(fontWeight: FontWeight.w800))))
                  .toList(),
            ),
          ),
        ]),
      ),
    );
  }
}

class _OfflineSafePage extends StatelessWidget {
  const _OfflineSafePage({required this.snapshot, required this.onRetry});
  final HomeSnapshot snapshot;
  final Future<void> Function() onRetry;

  @override
  Widget build(BuildContext context) => _DetailScaffold(
        title: '오프라인 안전 모드',
        child: ListView(padding: const EdgeInsets.all(20), children: [
          _NoticeBox(
              icon: Icons.cloud_off_outlined,
              title: 'Pi4 또는 네트워크 연결을 확인하는 중이에요',
              detail:
                  '마지막 상태는 ${_clock(snapshot.lastUpdated)}에 받았습니다. 화면은 마지막 정보를 그대로 유지합니다.',
              color: _amber),
          const SizedBox(height: 14),
          const Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Expanded(
              child: _InfoSection(
                  icon: Icons.check_circle_outline,
                  title: '계속 사용할 수 있어요',
                  lines: ['Pi5 로컬 사람 감지', '이미 저장된 안전 규칙', '직접 연결된 기기 제어'],
                  accent: _mint),
            ),
            SizedBox(width: 12),
            Expanded(
              child: _InfoSection(
                  icon: Icons.hourglass_top,
                  title: '잠시 제한돼요',
                  lines: ['Pi4 새 Task와 기록', '원격 알림', '외부 AI 및 연동 서비스'],
                  accent: _amber),
            ),
          ]),
          const SizedBox(height: 16),
          FilledButton.icon(
              onPressed: onRetry,
              icon: const Icon(Icons.refresh),
              label: const Text('지금 다시 연결')),
        ]),
      );
}

/// Whether the demo tools may offer the P0 test alert (mock or anonymous development broker only).
bool canTestSafetyAlert(HomeMindDataSource dataSource) =>
    dataSource is! MqttHomeMindDataSource || dataSource.auth.anonymous;
