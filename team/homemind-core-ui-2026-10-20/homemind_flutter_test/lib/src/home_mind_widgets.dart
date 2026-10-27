part of 'home_mind_app.dart';

class _StatusPill extends StatelessWidget {
  const _StatusPill(
      {required this.icon,
      required this.label,
      required this.color,
      this.shrinkable = false});
  final IconData icon;
  final String label;
  final Color color;

  /// Ellipsize the label; only inside a width-bounded parent (Flexible).
  final bool shrinkable;

  @override
  Widget build(BuildContext context) {
    final text = Text(label,
        maxLines: shrinkable ? 1 : null,
        overflow: shrinkable ? TextOverflow.ellipsis : null,
        style: TextStyle(color: color, fontWeight: FontWeight.w800));
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 11, vertical: 7),
      decoration: BoxDecoration(
        color: color.withValues(alpha: .11),
        borderRadius: BorderRadius.circular(20),
        border: Border.all(color: color.withValues(alpha: .48)),
      ),
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        Icon(icon, color: color, size: 17),
        const SizedBox(width: 6),
        if (shrinkable) Flexible(child: text) else text,
      ]),
    );
  }
}

/// Push-to-talk in the header: 말하기 → 듣는 중… (press again to cancel).
/// Disabled while offline or while privacy mode turns the microphone off.
class _TalkButton extends StatelessWidget {
  const _TalkButton(
      {required this.listening,
      required this.connected,
      required this.microphoneAllowed,
      required this.onPressed});
  final bool listening;
  final bool connected;
  final bool microphoneAllowed;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    final enabled = listening || (connected && microphoneAllowed);
    final tooltip = listening
        ? '다시 누르면 듣기를 멈춰요'
        : !microphoneAllowed
            ? '개인정보 모드에서 마이크가 꺼져 있어요'
            : !connected
                ? 'Pi4에 다시 연결되면 말할 수 있어요'
                : '누르고 한 번 말해 주세요';
    return Tooltip(
      message: tooltip,
      child: FilledButton.icon(
        style: FilledButton.styleFrom(
          minimumSize: const Size(124, 44),
          padding: const EdgeInsets.symmetric(horizontal: 16),
          backgroundColor: listening ? _purple : _mint,
          foregroundColor: _navy,
        ),
        onPressed: enabled ? onPressed : null,
        icon: Icon(listening
            ? Icons.graphic_eq
            : microphoneAllowed
                ? Icons.mic
                : Icons.mic_off),
        label: Text(listening ? '듣는 중…' : '말하기',
            style: const TextStyle(fontSize: 17, fontWeight: FontWeight.w900)),
      ),
    );
  }
}

class _Panel extends StatelessWidget {
  const _Panel(
      {required this.title, required this.subtitle, required this.child});
  final String title;
  final String subtitle;
  final Widget child;

  @override
  Widget build(BuildContext context) => Card(
        margin: EdgeInsets.zero,
        child: Padding(
          padding: const EdgeInsets.all(16),
          child:
              Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Text(title,
                style:
                    const TextStyle(fontSize: 20, fontWeight: FontWeight.w900)),
            Text(subtitle,
                style: const TextStyle(color: Colors.white54, fontSize: 14)),
            const SizedBox(height: 10),
            Expanded(child: child),
          ]),
        ),
      );
}

class _MiniStateCard extends StatelessWidget {
  const _MiniStateCard(
      {required this.icon,
      required this.label,
      required this.value,
      required this.color});
  final IconData icon;
  final String label;
  final String value;
  final Color color;

  @override
  Widget build(BuildContext context) => Card(
        margin: EdgeInsets.zero,
        child: Padding(
          padding: const EdgeInsets.symmetric(horizontal: 13, vertical: 11),
          child: Row(children: [
            Icon(icon, color: color, size: 27),
            const SizedBox(width: 10),
            Expanded(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(label,
                      style:
                          const TextStyle(color: Colors.white54, fontSize: 13)),
                  Text(value,
                      style: TextStyle(
                          color: color,
                          fontSize: 20,
                          fontWeight: FontWeight.w900)),
                ],
              ),
            ),
          ]),
        ),
      );
}

class _CompactTaskCard extends StatelessWidget {
  const _CompactTaskCard(
      {required this.task, required this.dataSource, required this.timeValid});
  final HomeTask task;
  final HomeMindDataSource dataSource;
  final bool timeValid;

  @override
  Widget build(BuildContext context) {
    final needsDecision = task.state == HomeTaskState.proposed;
    final color = _taskStateColor(task.state);
    return Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
      Row(children: [
        CircleAvatar(
          backgroundColor: color.withValues(alpha: .14),
          child: Icon(_taskStateIcon(task.state), color: color),
        ),
        const SizedBox(width: 10),
        Expanded(
          child: Text(task.title,
              maxLines: 2,
              overflow: TextOverflow.ellipsis,
              style:
                  const TextStyle(fontSize: 18, fontWeight: FontWeight.w900)),
        ),
        _StatusPill(
            icon: _taskStateIcon(task.state),
            label: task.state.label,
            color: color),
      ]),
      const SizedBox(height: 8),
      Row(children: [
        _OriginBadge(origin: task.originKind),
        const SizedBox(width: 8),
        Expanded(
          child: Text(task.detail,
              style: const TextStyle(color: Colors.white70, height: 1.35),
              maxLines: 2,
              overflow: TextOverflow.ellipsis),
        ),
      ]),
      if (task.failure != null) ...[
        const SizedBox(height: 8),
        _FailureBox(failure: task.failure!, compact: true),
      ],
      const Spacer(),
      if (needsDecision)
        Row(children: [
          Expanded(
            child: FilledButton.icon(
              onPressed: () =>
                  dataSource.actOnTask(task.id, TaskAction.approve),
              icon: const Icon(Icons.check),
              label: const Text('승인'),
            ),
          ),
          const SizedBox(width: 9),
          Expanded(
            child: OutlinedButton.icon(
              onPressed: () => dataSource.actOnTask(task.id, TaskAction.reject),
              icon: const Icon(Icons.close),
              label: const Text('거절'),
            ),
          ),
        ])
      else
        OutlinedButton.icon(
          onPressed: () => _openPage(
              context,
              _TaskDetailPage(
                  task: task, dataSource: dataSource, timeValid: timeValid)),
          icon: const Icon(Icons.timeline),
          label: const Text('진행 과정 보기'),
        ),
    ]);
  }
}

class _DetailScaffold extends StatelessWidget {
  const _DetailScaffold(
      {required this.title, required this.child, this.actions = const []});
  final String title;
  final Widget child;
  final List<Widget> actions;

  @override
  Widget build(BuildContext context) => Scaffold(
        appBar: AppBar(
          backgroundColor: const Color(0xFF111C2C),
          title:
              Text(title, style: const TextStyle(fontWeight: FontWeight.w900)),
          actions: actions,
        ),
        body: SafeArea(child: child),
      );
}

class _InfoSection extends StatelessWidget {
  const _InfoSection(
      {required this.icon,
      required this.title,
      required this.lines,
      this.accent = _mint});
  final IconData icon;
  final String title;
  final List<String> lines;
  final Color accent;

  @override
  Widget build(BuildContext context) => Card(
        margin: EdgeInsets.zero,
        child: Padding(
          padding: const EdgeInsets.all(17),
          child:
              Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
            Row(children: [
              Icon(icon, color: accent, size: 28),
              const SizedBox(width: 9),
              Text(title,
                  style: const TextStyle(
                      fontSize: 18, fontWeight: FontWeight.w900)),
            ]),
            const SizedBox(height: 10),
            for (final line in lines)
              Padding(
                padding: const EdgeInsets.only(bottom: 6),
                child: Row(children: [
                  Icon(Icons.check_circle_outline, color: accent, size: 17),
                  const SizedBox(width: 7),
                  Expanded(child: Text(line)),
                ]),
              ),
          ]),
        ),
      );
}

class _NoticeBox extends StatelessWidget {
  const _NoticeBox(
      {required this.icon,
      required this.title,
      required this.detail,
      required this.color});
  final IconData icon;
  final String title;
  final String detail;
  final Color color;

  @override
  Widget build(BuildContext context) => Container(
        width: double.infinity,
        padding: const EdgeInsets.all(13),
        decoration: BoxDecoration(
          color: color.withValues(alpha: .1),
          borderRadius: BorderRadius.circular(11),
          border: Border.all(color: color.withValues(alpha: .45)),
        ),
        child: Row(crossAxisAlignment: CrossAxisAlignment.start, children: [
          Icon(icon, color: color),
          const SizedBox(width: 10),
          Expanded(
            child:
                Column(crossAxisAlignment: CrossAxisAlignment.start, children: [
              Text(title,
                  style: TextStyle(color: color, fontWeight: FontWeight.w900)),
              const SizedBox(height: 3),
              Text(detail,
                  style: const TextStyle(color: Colors.white70, height: 1.35)),
            ]),
          ),
        ]),
      );
}

class _MetricCard extends StatelessWidget {
  const _MetricCard(
      {required this.icon,
      required this.label,
      required this.value,
      required this.color});
  final IconData icon;
  final String label;
  final String value;
  final Color color;

  @override
  Widget build(BuildContext context) => Card(
        margin: EdgeInsets.zero,
        child: Padding(
          padding: const EdgeInsets.all(13),
          child: Row(children: [
            Icon(icon, color: color, size: 29),
            const SizedBox(width: 9),
            Expanded(
              child: Column(
                mainAxisAlignment: MainAxisAlignment.center,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(label,
                      style:
                          const TextStyle(color: Colors.white54, fontSize: 13)),
                  Text(value,
                      style: TextStyle(
                          color: color,
                          fontSize: 17,
                          fontWeight: FontWeight.w900),
                      overflow: TextOverflow.ellipsis),
                ],
              ),
            ),
          ]),
        ),
      );
}

class _EmptyState extends StatelessWidget {
  const _EmptyState(
      {required this.icon, required this.title, required this.detail});
  final IconData icon;
  final String title;
  final String detail;

  @override
  Widget build(BuildContext context) => Center(
        child: Column(mainAxisSize: MainAxisSize.min, children: [
          Icon(icon, color: Colors.white30, size: 45),
          const SizedBox(height: 9),
          Text(title,
              style:
                  const TextStyle(fontSize: 18, fontWeight: FontWeight.w900)),
          const SizedBox(height: 4),
          Text(detail,
              textAlign: TextAlign.center,
              style: const TextStyle(color: Colors.white54)),
        ]),
      );
}

class _KeyValue extends StatelessWidget {
  const _KeyValue({required this.label, required this.value});
  final String label;
  final String value;

  @override
  Widget build(BuildContext context) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 6),
        child: Row(children: [
          SizedBox(
              width: 140,
              child:
                  Text(label, style: const TextStyle(color: Colors.white54))),
          Expanded(
              child: Text(value,
                  style: const TextStyle(fontWeight: FontWeight.w800))),
        ]),
      );
}

/// 음성 명령 / AI 제안 / 휴대폰 / 상황 감지 / 화면.
class _OriginBadge extends StatelessWidget {
  const _OriginBadge({required this.origin});
  final TaskOrigin origin;

  @override
  Widget build(BuildContext context) {
    if (origin == TaskOrigin.unknown) return const SizedBox.shrink();
    final color = _originColor(origin);
    return Container(
      padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
      decoration: BoxDecoration(
        color: color.withValues(alpha: .16),
        borderRadius: BorderRadius.circular(6),
      ),
      child: Row(mainAxisSize: MainAxisSize.min, children: [
        Icon(_originIcon(origin), color: color, size: 14),
        const SizedBox(width: 4),
        Text(origin.label,
            style: TextStyle(
                color: color, fontSize: 12, fontWeight: FontWeight.w800)),
      ]),
    );
  }
}

/// Korean failure reason; the raw code stays visible in small text.
class _FailureBox extends StatelessWidget {
  const _FailureBox({required this.failure, this.compact = false, this.time});
  final TaskFailure failure;
  final bool compact;
  final String? time;

  @override
  Widget build(BuildContext context) {
    final retry = failure.retryAllowed
        ? '연결 상태를 확인한 뒤 다시 시도할 수 있어요.'
        : '다시 시도해도 같은 이유로 실행되지 않아요.';
    final label = failureShortLabel(failure.code);
    final title = time == null ? label : '$label · $time';
    final detail = compact
        ? failureExplanation(failure)
        : '${failureExplanation(failure)}\n$retry\n'
            '코드 ${failure.code} · ${failure.message}';
    return _NoticeBox(
        icon: Icons.warning_amber_rounded,
        title: title,
        detail: detail,
        color: _amber);
  }
}

/// Scenario A: shown while the person is still speaking and the partial text already points at an intent.
/// Display only (execution_allowed is false); the task comes from the final utterance.
class _PartialHintBanner extends StatelessWidget {
  const _PartialHintBanner({required this.hint});
  final PartialIntentHint hint;

  @override
  Widget build(BuildContext context) {
    final devices = hint.candidateDevices
        .map((id) => officialDeviceById(id)?.shortName ?? id)
        .join(' · ');
    return Container(
      height: 44,
      padding: const EdgeInsets.symmetric(horizontal: 20),
      color: const Color(0xFF16323A),
      child: Row(children: [
        const Icon(Icons.hearing, color: _mint),
        const SizedBox(width: 10),
        Text('말씀을 듣는 중 · ${hint.intentLabel} 준비',
            style: const TextStyle(
                color: _mint, fontSize: 16, fontWeight: FontWeight.w900)),
        if (devices.isNotEmpty) ...[
          const SizedBox(width: 12),
          Flexible(
            child: Text(devices,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(color: Colors.white70)),
          ),
        ],
        const Spacer(),
        const Text('말이 끝나면 확인해요 · 아직 실행하지 않아요',
            style: TextStyle(color: Colors.white54)),
      ]),
    );
  }
}

/// Shown under the header while quiet mode is on.
class _QuietModeBanner extends StatelessWidget {
  const _QuietModeBanner({required this.snapshot, required this.dataSource});
  final HomeSnapshot snapshot;
  final HomeMindDataSource dataSource;

  @override
  Widget build(BuildContext context) {
    final quiet = snapshot.quietMode;
    final activity = snapshot.activity;
    final title = quiet.byTask ? '조용 모드 켜짐' : '조용히 해야 하는 상황';
    final situation = activity == null
        ? ''
        : ' · ${activity.activityLabel} · ${activity.audioLabel}';
    return Container(
      height: 44,
      padding: const EdgeInsets.symmetric(horizontal: 20),
      color: const Color(0xFF2B2447),
      child: Row(children: [
        const Icon(Icons.volume_off_outlined, color: _purple),
        const SizedBox(width: 10),
        Flexible(
          child: Text('$title$situation',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
              style: const TextStyle(
                  color: _purple, fontSize: 16, fontWeight: FontWeight.w900)),
        ),
        const SizedBox(width: 12),
        _OriginBadge(origin: quiet.origin),
        const Spacer(),
        Text(quiet.byTask ? '에어컨 저소음 표시' : '조용 모드 준비 중',
            style: const TextStyle(color: Colors.white70)),
        if (quiet.byTask) ...[
          const SizedBox(width: 12),
          // Scenario C: a wrong context is undone at once; core keeps it off for the rest of that context.
          TextButton.icon(
            style: TextButton.styleFrom(
                foregroundColor: _amber,
                visualDensity: VisualDensity.compact,
                padding: const EdgeInsets.symmetric(horizontal: 10)),
            onPressed: () async {
              await dataSource.submitFeedback(
                  type: FeedbackType.context,
                  rating: FeedbackRating.negative,
                  note: 'quiet banner: 상황이 달라요');
              if (!context.mounted) return;
              ScaffoldMessenger.of(context).showSnackBar(const SnackBar(
                  content: Text('조용 모드를 끄고, 이 상황에서는 다시 켜지 않아요. 판단 기록은 Pi4에 남겨요.')));
            },
            icon: const Icon(Icons.thumb_down_alt_outlined, size: 18),
            label: const Text('상황이 달라요',
                style: TextStyle(fontWeight: FontWeight.w800)),
          ),
        ],
      ]),
    );
  }
}

/// Prominent approval card: "에어컨을 켤까요?" with 실행 / 취소.
class _ProposalCard extends StatefulWidget {
  const _ProposalCard(
      {super.key,
      required this.task,
      required this.dataSource,
      required this.timeValid,
      this.morePending = 0});
  final HomeTask task;
  final HomeMindDataSource dataSource;
  final bool timeValid;
  final int morePending;

  @override
  State<_ProposalCard> createState() => _ProposalCardState();
}

class _ProposalCardState extends State<_ProposalCard> {
  String? _sentFor;

  bool get _sending => _sentFor == '${widget.task.id}:${widget.task.revision}';

  Future<void> _act(TaskAction action) async {
    if (_sending) return;
    setState(() => _sentFor = '${widget.task.id}:${widget.task.revision}');
    await widget.dataSource.actOnTask(widget.task.id, action);
  }

  @override
  Widget build(BuildContext context) {
    final task = widget.task;
    final ai = task.originKind == TaskOrigin.ai;
    final accent = ai ? _purple : _amber;
    return Material(
      color: Colors.transparent,
      child: InkWell(
        borderRadius: BorderRadius.circular(16),
        onTap: () => _openPage(
            context,
            _TaskDetailPage(
                task: task,
                dataSource: widget.dataSource,
                timeValid: widget.timeValid)),
        child: Ink(
          padding: const EdgeInsets.all(14),
          decoration: BoxDecoration(
            color: accent.withValues(alpha: .10),
            borderRadius: BorderRadius.circular(16),
            border: Border.all(color: accent.withValues(alpha: .6), width: 1.5),
          ),
          child:
              Column(crossAxisAlignment: CrossAxisAlignment.stretch, children: [
            Row(children: [
              _OriginBadge(origin: task.originKind),
              const Spacer(),
              if (task.confidence > 0)
                Text('신뢰도 ${(task.confidence * 100).round()}%',
                    style:
                        const TextStyle(color: Colors.white54, fontSize: 13)),
            ]),
            const SizedBox(height: 10),
            Flexible(
              child: Text(taskQuestion(task),
                  maxLines: 2,
                  overflow: TextOverflow.ellipsis,
                  style: const TextStyle(
                      fontSize: 26, height: 1.25, fontWeight: FontWeight.w900)),
            ),
            const SizedBox(height: 6),
            Text(task.detail,
                maxLines: 1,
                overflow: TextOverflow.ellipsis,
                style: const TextStyle(color: Colors.white60)),
            if (widget.morePending > 0)
              Text('확인 대기 ${widget.morePending}건 더 있어요 · Task 탭',
                  style: const TextStyle(color: Colors.white38, fontSize: 13)),
            const Spacer(),
            Row(children: [
              Expanded(
                child: FilledButton.icon(
                  style: FilledButton.styleFrom(
                      minimumSize: const Size(48, 54),
                      backgroundColor: accent,
                      foregroundColor: _navy),
                  onPressed: _sending ? null : () => _act(TaskAction.approve),
                  icon: const Icon(Icons.check),
                  label: const Text('실행',
                      style:
                          TextStyle(fontSize: 18, fontWeight: FontWeight.w900)),
                ),
              ),
              const SizedBox(width: 10),
              Expanded(
                child: OutlinedButton.icon(
                  style: OutlinedButton.styleFrom(
                      minimumSize: const Size(48, 54)),
                  onPressed: _sending ? null : () => _act(TaskAction.reject),
                  icon: const Icon(Icons.close),
                  label: const Text('취소',
                      style:
                          TextStyle(fontSize: 18, fontWeight: FontWeight.w800)),
                ),
              ),
            ]),
          ]),
        ),
      ),
    );
  }
}

/// Latest ai/llm/result that needs words on screen (clarify question etc.).
class _AiMessageBar extends StatelessWidget {
  const _AiMessageBar({required this.result, required this.onDismiss});
  final LlmResultInfo result;
  final VoidCallback onDismiss;

  @override
  Widget build(BuildContext context) => Container(
        padding: const EdgeInsets.fromLTRB(14, 8, 4, 8),
        decoration: BoxDecoration(
          color: const Color(0xFF2B2447),
          borderRadius: BorderRadius.circular(14),
          border: Border.all(color: _purple.withValues(alpha: .55)),
        ),
        child: Row(children: [
          Icon(result.isQuestion ? Icons.record_voice_over : Icons.info_outline,
              color: _purple, size: 28),
          const SizedBox(width: 12),
          Expanded(
            child: Column(
                mainAxisSize: MainAxisSize.min,
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(result.isQuestion ? 'HomeMind가 물어봐요' : 'AI 응답',
                      style: const TextStyle(
                          color: _purple,
                          fontSize: 13,
                          fontWeight: FontWeight.w800)),
                  Text(result.message ?? '',
                      maxLines: 2,
                      overflow: TextOverflow.ellipsis,
                      style: const TextStyle(
                          fontSize: 18, fontWeight: FontWeight.w900)),
                ]),
          ),
          IconButton(
              tooltip: '메시지 닫기',
              onPressed: onDismiss,
              icon: const Icon(Icons.close, color: Colors.white54)),
        ]),
      );
}
