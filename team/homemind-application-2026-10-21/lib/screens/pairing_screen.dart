import 'package:flutter/foundation.dart';
import 'package:flutter/material.dart';

import '../core/api/homemind_api.dart';
import '../core/theme/homemind_theme.dart';
import '../models/pairing_models.dart';
import '../state/app_controller.dart';
import '../widgets/common_widgets.dart';
import 'qr_scanner_screen.dart';

class PairingScreen extends StatefulWidget {
  const PairingScreen({
    super.key,
    required this.controller,
    this.initialSetup = false,
    this.onDemoRequested,
    this.onDevelopmentConnected,
    this.onPaired,
  });

  final AppController controller;
  final bool initialSetup;
  final VoidCallback? onDemoRequested;
  final VoidCallback? onDevelopmentConnected;

  /// Called after the claim succeeded and the token is stored. Without it the
  /// screen pops itself (opened from Settings).
  final VoidCallback? onPaired;

  @override
  State<PairingScreen> createState() => _PairingScreenState();
}

class _PairingScreenState extends State<PairingScreen> {
  late final TextEditingController _urlController;
  late final TextEditingController _qrTextController;
  PairingPayload? _payload;
  String? _qrError;
  bool _readyForClaim = false;
  bool _claiming = false;
  bool _testing = false;

  @override
  void initState() {
    super.initState();
    _urlController = TextEditingController(text: widget.controller.baseUrl);
    _qrTextController = TextEditingController();
  }

  @override
  void dispose() {
    _urlController.dispose();
    _qrTextController.dispose();
    super.dispose();
  }

  Future<void> _scanQr() async {
    final rawValue = await Navigator.of(context).push<String>(
      MaterialPageRoute<String>(builder: (_) => const QrScannerScreen()),
    );
    if (!mounted || rawValue == null) return;
    _readQr(rawValue);
  }

  void _readQr(String rawValue) {
    try {
      final payload = PairingPayload.parse(rawValue);
      setState(() {
        _payload = payload;
        _readyForClaim = false;
        _qrError = null;
        _urlController.text = payload.baseUrl;
      });
    } on PairingPayloadException catch (error) {
      setState(() {
        _payload = null;
        _readyForClaim = false;
        _qrError = error.message;
      });
    }
  }

  Future<void> _approvePairing() async {
    final payload = _payload;
    if (payload == null) return;
    if (payload.isExpiredAt(DateTime.now())) {
      setState(() {
        _readyForClaim = false;
        _qrError = '승인 전에 QR이 만료되었습니다. 새 QR을 스캔해주세요.';
      });
      return;
    }

    final approved = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: const Text('이 HomeMind에 연결할까요?'),
        content: Text(
          '${payload.deviceName}\n${payload.baseUrl}\n\n'
          '기기 화면의 코드가 ${payload.pairingCode}와 같은지 확인하세요. '
          '승인하면 일회용 코드를 보내 이 휴대폰 전용 토큰을 받습니다.',
        ),
        actions: [
          TextButton(
            onPressed: () => Navigator.of(context).pop(false),
            child: const Text('취소'),
          ),
          FilledButton(
            onPressed: () => Navigator.of(context).pop(true),
            child: const Text('승인'),
          ),
        ],
      ),
    );
    if (!mounted || approved != true) return;
    setState(() {
      _readyForClaim = true;
      _claiming = true;
      _qrError = null;
    });
    try {
      await widget.controller.completePairing(payload);
    } catch (error) {
      if (!mounted) return;
      setState(() {
        _claiming = false;
        _readyForClaim = false;
        _qrError = error is HomeMindApiException
            ? error.message
            : '연결 정보를 저장하지 못했습니다 ($error).';
      });
      return;
    }
    if (!mounted) return;
    setState(() => _claiming = false);
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(content: Text('${payload.deviceName}에 연결했습니다.')),
    );
    if (widget.onPaired != null) {
      widget.onPaired!();
    } else {
      Navigator.of(context).pop();
    }
  }

  Future<void> _connectDirectly() async {
    setState(() => _testing = true);
    final success = await widget.controller.updateBaseUrl(_urlController.text);
    if (!mounted) return;
    setState(() => _testing = false);
    ScaffoldMessenger.of(context).showSnackBar(
      SnackBar(
        content: Text(
          success ? '개발용 백엔드에 직접 연결했습니다.' : '연결하지 못해 데모 모드를 유지합니다.',
        ),
      ),
    );
    if (!success) return;
    if (widget.onDevelopmentConnected != null) {
      widget.onDevelopmentConnected!();
    } else {
      Navigator.of(context).pop();
    }
  }

  @override
  Widget build(BuildContext context) {
    return Scaffold(
      appBar: AppBar(
        automaticallyImplyLeading: !widget.initialSetup,
        title: Text(widget.initialSetup ? 'HomeMind 시작' : '기기 연결'),
        backgroundColor: HomeMindColors.background,
      ),
      body: ScreenFrame(
        children: [
          const SectionHeading(
            title: 'HomeMind 연결',
            subtitle: '기기에서 만든 QR을 스캔하고 승인하면 이 휴대폰이 HomeMind에 연결됩니다.',
          ),
          if (widget.controller.needsPairing &&
              widget.controller.lastError != null) ...[
            const SizedBox(height: 14),
            _PairingError(message: widget.controller.lastError!),
          ],
          const SizedBox(height: 24),
          if (_payload == null)
            _ScanCard(onScan: _scanQr)
          else
            _PairingCandidateCard(
              payload: _payload!,
              readyForClaim: _readyForClaim,
              claiming: _claiming,
              onApprove: _approvePairing,
              onRescan: _scanQr,
            ),
          if (_qrError != null) ...[
            const SizedBox(height: 14),
            _PairingError(message: _qrError!),
          ],
          if (_claiming) ...[
            const SizedBox(height: 16),
            const HomeMindCard(
              accent: HomeMindColors.mint,
              child: Row(
                children: [
                  SizedBox(
                    width: 22,
                    height: 22,
                    child: CircularProgressIndicator(strokeWidth: 2),
                  ),
                  SizedBox(width: 12),
                  Expanded(child: Text('연결 중: 일회용 코드로 토큰을 받는 중입니다.')),
                ],
              ),
            ),
          ],
          if (kDebugMode) ...[
            if (widget.initialSetup && widget.onDemoRequested != null) ...[
              const SizedBox(height: 18),
              HomeMindCard(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      '개발 중인가요?',
                      style: Theme.of(context).textTheme.titleMedium,
                    ),
                    const SizedBox(height: 7),
                    Text(
                      'Pi 4 없이 UI를 확인할 때만 데모 데이터를 사용합니다.',
                      style: Theme.of(context).textTheme.bodyMedium,
                    ),
                    const SizedBox(height: 12),
                    OutlinedButton.icon(
                      onPressed: widget.onDemoRequested,
                      icon: const Icon(Icons.visibility_outlined),
                      label: const Text('데모 모드로 둘러보기'),
                    ),
                  ],
                ),
              ),
            ],
            const SizedBox(height: 18),
            _ManualQrInput(
              controller: _qrTextController,
              onParse: () => _readQr(_qrTextController.text),
            ),
            const SizedBox(height: 24),
            const Divider(color: HomeMindColors.border),
            const SizedBox(height: 20),
            Text('개발용 직접 연결', style: Theme.of(context).textTheme.titleMedium),
            const SizedBox(height: 7),
            Text(
              '디버그 기능입니다. 서버는 페어링 토큰을 요구하므로 페어링하지 않은 주소는 401로 거절됩니다. 토큰은 페어링한 주소에만 보냅니다.',
              style: Theme.of(context).textTheme.bodyMedium,
            ),
            const SizedBox(height: 12),
            TextField(
              controller: _urlController,
              keyboardType: TextInputType.url,
              autocorrect: false,
              decoration: const InputDecoration(
                hintText: 'http://192.168.1.10:8000',
                prefixIcon: Icon(Icons.link),
              ),
            ),
            const SizedBox(height: 14),
            PrimaryActionButton(
              label: _testing ? '연결 확인 중' : '주소로 직접 연결',
              icon: _testing ? Icons.sync : Icons.cable,
              expanded: false,
              onPressed: _testing ? null : _connectDirectly,
            ),
            const SizedBox(height: 18),
            Text(
              'Android 에뮬레이터 → Windows: http://10.0.2.2:8000\n'
              '실제 스마트폰 → Pi 4: http://<Pi-IP>:8000',
              style: Theme.of(context).textTheme.bodyMedium,
            ),
          ],
        ],
      ),
    );
  }
}

class _ScanCard extends StatelessWidget {
  const _ScanCard({required this.onScan});

  final VoidCallback onScan;

  @override
  Widget build(BuildContext context) {
    return HomeMindCard(
      accent: HomeMindColors.mint,
      child: Column(
        children: [
          Container(
            width: 170,
            height: 170,
            decoration: BoxDecoration(
              color: Colors.white,
              borderRadius: BorderRadius.circular(18),
            ),
            child: const Center(
              child: Icon(
                Icons.qr_code_scanner,
                color: HomeMindColors.background,
                size: 96,
              ),
            ),
          ),
          const SizedBox(height: 18),
          Text(
            'Pi 4의 QR을 스캔하세요',
            style: Theme.of(context).textTheme.titleLarge,
          ),
          const SizedBox(height: 7),
          Text(
            '일회용 코드와 만료시간을 검사한 뒤 연결할 기기를 확인합니다.',
            textAlign: TextAlign.center,
            style: Theme.of(context).textTheme.bodyMedium,
          ),
          const SizedBox(height: 18),
          PrimaryActionButton(
            label: 'QR 코드 스캔',
            icon: Icons.camera_alt_outlined,
            expanded: false,
            onPressed: onScan,
          ),
        ],
      ),
    );
  }
}

class _PairingCandidateCard extends StatelessWidget {
  const _PairingCandidateCard({
    required this.payload,
    required this.readyForClaim,
    required this.claiming,
    required this.onApprove,
    required this.onRescan,
  });

  final PairingPayload payload;
  final bool readyForClaim;
  final bool claiming;
  final VoidCallback onApprove;
  final VoidCallback onRescan;

  @override
  Widget build(BuildContext context) {
    return HomeMindCard(
      accent: readyForClaim ? HomeMindColors.green : HomeMindColors.mint,
      child: Column(
        crossAxisAlignment: CrossAxisAlignment.start,
        children: [
          Row(
            children: [
              Icon(
                readyForClaim ? Icons.verified_outlined : Icons.hub_outlined,
                color: readyForClaim
                    ? HomeMindColors.green
                    : HomeMindColors.mint,
                size: 38,
              ),
              const SizedBox(width: 13),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Text(
                      payload.deviceName,
                      style: Theme.of(context).textTheme.titleLarge,
                    ),
                    const SizedBox(height: 4),
                    Text(
                      payload.deviceId,
                      style: Theme.of(context).textTheme.bodyMedium,
                    ),
                  ],
                ),
              ),
              StatusPill(
                label: readyForClaim ? '승인 완료' : 'QR 확인됨',
                color: readyForClaim
                    ? HomeMindColors.green
                    : HomeMindColors.mint,
              ),
            ],
          ),
          const Divider(height: 28, color: HomeMindColors.border),
          _InfoRow(label: 'Pi 주소', value: payload.baseUrl),
          const SizedBox(height: 9),
          _InfoRow(label: '일회용 코드', value: payload.pairingCode),
          const SizedBox(height: 9),
          _InfoRow(label: '만료', value: _formatDate(payload.expiresAt)),
          const SizedBox(height: 9),
          _InfoRow(
            label: '보안 연결',
            value: payload.tlsSha256 != null
                ? 'HTTPS · 인증서 ${payload.tlsSha256!.substring(0, 8)}… 고정'
                : payload.baseUri.scheme == 'https'
                ? 'HTTPS'
                : '암호화 없음(개발용 http)',
          ),
          const SizedBox(height: 18),
          Row(
            children: [
              Expanded(
                child: FilledButton.icon(
                  onPressed: readyForClaim ? null : onApprove,
                  icon: const Icon(Icons.link),
                  label: Text(readyForClaim ? '승인 완료' : '이 기기에 연결'),
                ),
              ),
              const SizedBox(width: 10),
              OutlinedButton(
                onPressed: claiming ? null : onRescan,
                child: const Text('다시 스캔'),
              ),
            ],
          ),
        ],
      ),
    );
  }

  static String _formatDate(DateTime value) {
    final local = value.toLocal();
    String two(int number) => number.toString().padLeft(2, '0');
    return '${local.year}.${two(local.month)}.${two(local.day)} '
        '${two(local.hour)}:${two(local.minute)}:${two(local.second)}';
  }
}

class _InfoRow extends StatelessWidget {
  const _InfoRow({required this.label, required this.value});

  final String label;
  final String value;

  @override
  Widget build(BuildContext context) {
    return Row(
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        SizedBox(
          width: 92,
          child: Text(label, style: Theme.of(context).textTheme.bodyMedium),
        ),
        Expanded(
          child: Text(
            value,
            style: Theme.of(context).textTheme.bodyLarge
                ?.copyWith(fontWeight: FontWeight.w700),
          ),
        ),
      ],
    );
  }
}

class _ManualQrInput extends StatelessWidget {
  const _ManualQrInput({required this.controller, required this.onParse});

  final TextEditingController controller;
  final VoidCallback onParse;

  @override
  Widget build(BuildContext context) {
    return HomeMindCard(
      child: ExpansionTile(
        tilePadding: EdgeInsets.zero,
        childrenPadding: const EdgeInsets.only(top: 8),
        title: Text(
          '에뮬레이터용 QR 문자열 입력',
          style: Theme.of(context).textTheme.titleMedium,
        ),
        subtitle: Text(
          '카메라 대신 QR의 JSON 내용을 붙여 넣어 검사합니다.',
          style: Theme.of(context).textTheme.bodyMedium,
        ),
        children: [
          TextField(
            controller: controller,
            minLines: 5,
            maxLines: 8,
            autocorrect: false,
            decoration: const InputDecoration(hintText: '{"version": 1, ...}'),
          ),
          const SizedBox(height: 12),
          Align(
            alignment: Alignment.centerLeft,
            child: PrimaryActionButton(
              label: 'QR 내용 검사',
              icon: Icons.fact_check_outlined,
              expanded: false,
              onPressed: onParse,
            ),
          ),
        ],
      ),
    );
  }
}

class _PairingError extends StatelessWidget {
  const _PairingError({required this.message});

  final String message;

  @override
  Widget build(BuildContext context) {
    return Container(
      padding: const EdgeInsets.all(14),
      decoration: BoxDecoration(
        color: HomeMindColors.red.withValues(alpha: 0.08),
        border: Border.all(color: HomeMindColors.red.withValues(alpha: 0.5)),
        borderRadius: BorderRadius.circular(14),
      ),
      child: Row(
        children: [
          const Icon(Icons.error_outline, color: HomeMindColors.red),
          const SizedBox(width: 10),
          Expanded(child: Text(message)),
        ],
      ),
    );
  }
}
