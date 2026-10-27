import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';

import '../core/api/homemind_api.dart';
import '../core/auth/pairing_credentials.dart';
import '../models/approval_models.dart';
import '../models/hardware_models.dart';
import '../models/pairing_models.dart';

class AppController extends ChangeNotifier {
  AppController({HomeMindApi? api, CredentialStore? credentials})
    : _api = api ?? HomeMindApi(),
      _credentials = credentials ?? SecureCredentialStore();

  final HomeMindApi _api;
  final CredentialStore _credentials;
  HardwareSnapshot snapshot = HardwareDemoData.snapshot;
  bool isLoading = false;
  bool usingDemoData = true;
  String? lastError;
  String? lastMessage;
  int selectedIndex = 0;
  Timer? _refreshTimer;

  /// The paired HomeMind device (null until a QR pairing succeeded).
  PairingCredential? credential;

  /// Set when the server answered 401: the app must go back to QR pairing.
  bool needsPairing = false;

  /// Tasks waiting for approval; demo items when no server is paired.
  List<ApprovalItem> get approvals =>
      !_api.hasToken && usingDemoData ? _demoApprovals : _approvals;
  List<ApprovalItem> _approvals = const [];
  List<ApprovalItem> _demoApprovals = ApprovalDemoData.items;

  /// A new request arrived while the app runs ("승인 요청: …"); cleared when the
  /// approvals tab opens.
  String? approvalAlert;

  /// The live event stream (GET /api/v1/events) is open.
  bool liveConnected = false;
  StreamSubscription<LiveEvent>? _liveSubscription;
  Timer? _liveRetry;
  bool _liveWanted = false;
  int? _lastEventSeq;

  static const approvalsTab = 1;

  String get baseUrl => _api.baseUrl;
  bool get connected => !usingDemoData && snapshot.connected;
  bool get paired => credential != null;

  /// Loads a stored pairing (app start). True when the phone is paired.
  Future<bool> restoreSession() async {
    final stored = await _credentials.read();
    if (stored == null) return false;
    _useCredential(stored);
    notifyListeners();
    return true;
  }

  /// QR approved by the user: claim the one-time code, keep the token in the
  /// secure store and connect. Throws [HomeMindPairingException] on refusal.
  Future<void> completePairing(PairingPayload payload, {String? phoneName}) async {
    final result = await _api.claimPairing(
      payload,
      phoneName: phoneName ?? _defaultPhoneName(),
    );
    final stored = PairingCredential(
      baseUrl: payload.baseUrl,
      token: result.token,
      tokenId: result.tokenId,
      deviceId: result.deviceId,
      deviceName: result.deviceName,
      tlsSha256: payload.tlsSha256,
    );
    await _credentials.write(stored);
    _useCredential(stored);
    needsPairing = false;
    lastError = null;
    lastMessage = '${stored.deviceName}에 연결했습니다.';
    notifyListeners();
    await refresh(silent: true);
  }

  /// Forgets this phone's token (Settings → 연결 해제). The server-side token
  /// stays until it is revoked on the device (--revoke-token).
  Future<void> forgetPairing() async {
    await _credentials.clear();
    credential = null;
    _api.setToken(null);
    _api.setTlsPin(null);
    _toPairing(null);
  }

  /// Dashboard every 15 s, plus live events; the approval list is polled too
  /// while the live stream is down.
  void startAutoRefresh() {
    _refreshTimer?.cancel();
    _refreshTimer = Timer.periodic(const Duration(seconds: 15), (_) {
      refresh(silent: true);
      if (!liveConnected) refreshApprovals();
    });
    startLiveEvents();
    refreshApprovals();
  }

  void selectTab(int index) {
    selectedIndex = index;
    if (index == approvalsTab) approvalAlert = null;
    notifyListeners();
  }

  void dismissApprovalAlert() {
    approvalAlert = null;
    notifyListeners();
  }

  Future<void> refreshApprovals() async {
    if (!_api.hasToken) {
      _approvals = const [];
      notifyListeners();
      return;
    }
    try {
      final list = await _api.getApprovals();
      final known = _approvals.map((item) => item.taskId).toSet();
      final fresh = list.where((item) => !known.contains(item.taskId)).toList();
      _approvals = list;
      if (fresh.isNotEmpty && selectedIndex != approvalsTab) {
        approvalAlert = fresh.length == 1
            ? '승인 요청: ${fresh.single.title}'
            : '승인 요청 ${fresh.length}건';
      }
      if (list.isEmpty) approvalAlert = null;
      notifyListeners();
    } on HomeMindUnauthorizedException catch (error) {
      await _unauthorized(error);
    } catch (_) {
      // keep the last list; the dashboard refresh reports connection errors
    }
  }

  /// Phone approve / reject. Core's answer arrives as a task change; the item
  /// leaves the list at once.
  Future<bool> decideApproval(ApprovalItem item, {required bool approve}) async {
    isLoading = true;
    notifyListeners();
    try {
      if (!_api.hasToken) {
        if (approve && !item.phoneCanApprove) {
          throw const HomeMindApiException('현관문 열기는 집 안 7인치 화면에서 승인해요.');
        }
        _demoApprovals = _demoApprovals
            .where((other) => other.taskId != item.taskId)
            .toList();
        lastMessage = '데모 모드: 실제로 보내지 않았습니다.';
      } else {
        await _api.decideTask(item.taskId, approve: approve);
        _approvals = _approvals
            .where((other) => other.taskId != item.taskId)
            .toList();
        lastMessage = approve
            ? '승인했습니다. 실행 결과는 장치 화면에 표시됩니다.'
            : '거절했습니다.';
      }
      lastError = null;
      return true;
    } on HomeMindUnauthorizedException catch (error) {
      await _unauthorized(error);
      return false;
    } catch (error) {
      lastError = error.toString();
      unawaited(refreshApprovals());
      return false;
    } finally {
      isLoading = false;
      notifyListeners();
    }
  }

  /// Opens GET /api/v1/events; reconnects 5 s after it drops.
  void startLiveEvents() {
    _liveWanted = true;
    _connectLive();
  }

  void stopLiveEvents() {
    _liveWanted = false;
    _liveRetry?.cancel();
    _liveRetry = null;
    _liveSubscription?.cancel();
    _liveSubscription = null;
    liveConnected = false;
  }

  void _connectLive() {
    _liveRetry?.cancel();
    _liveSubscription?.cancel();
    if (!_liveWanted || !_api.hasToken) return;
    _liveSubscription = _api
        .liveEvents(after: _lastEventSeq)
        .listen(
          _onLiveEvent,
          onError: (Object error) {
            if (error is HomeMindUnauthorizedException) {
              _unauthorized(error);
            } else {
              _liveLost();
            }
          },
          onDone: _liveLost,
          cancelOnError: true,
        );
  }

  void _liveLost() {
    liveConnected = false;
    notifyListeners();
    if (_liveWanted) {
      _liveRetry = Timer(const Duration(seconds: 5), _connectLive);
    }
  }

  void _onLiveEvent(LiveEvent event) {
    _lastEventSeq = event.seq ?? _lastEventSeq;
    switch (event.type) {
      case 'open':
        liveConnected = true;
        notifyListeners();
        unawaited(refreshApprovals());
      case 'approvals':
        unawaited(refreshApprovals());
      case 'task':
        if (const {'completed', 'failed', 'rejected', 'cancelled'}
            .contains(event.data['state'])) {
          unawaited(refresh(silent: true));
        }
    }
  }

  Future<bool> updateBaseUrl(String value) async {
    _api.baseUrl = value;
    notifyListeners();
    return refresh();
  }

  Future<bool> refresh({bool silent = false}) async {
    if (!silent) {
      isLoading = true;
      notifyListeners();
    }
    try {
      snapshot = await _api.getDashboard();
      usingDemoData = false;
      lastError = null;
      return true;
    } on HomeMindUnauthorizedException catch (error) {
      await _unauthorized(error);
      return false;
    } catch (error) {
      lastError = error.toString();
      return false;
    } finally {
      isLoading = false;
      notifyListeners();
    }
  }

  Future<bool> sendDeviceCommand(HardwareDevice device, String command) async {
    isLoading = true;
    notifyListeners();
    try {
      if (usingDemoData) {
        _replaceDevice(_simulateDeviceCommand(device, command));
        lastMessage = '데모 모드: 실제 장치에는 보내지 않았습니다.';
      } else {
        final result = await _api.sendDeviceCommand(device.id, command);
        _replaceDevice(result.device);
        lastMessage = result.message;
        if (result.taskId != null) {
          unawaited(_followTask(result.taskId!));
        }
      }
      lastError = null;
      return true;
    } on HomeMindUnauthorizedException catch (error) {
      await _unauthorized(error);
      return false;
    } catch (error) {
      lastError = error.toString();
      return false;
    } finally {
      isLoading = false;
      notifyListeners();
    }
  }

  /// Polls the core task until it finishes (ESP ACK or rejection), then reloads
  /// the dashboard so the confirmed state appears. Gives up after [timeout].
  Future<void> _followTask(
    String taskId, {
    Duration interval = const Duration(milliseconds: 500),
    Duration timeout = const Duration(seconds: 12),
  }) async {
    final deadline = DateTime.now().add(timeout);
    while (DateTime.now().isBefore(deadline)) {
      await Future<void>.delayed(interval);
      try {
        final status = await _api.getTask(taskId);
        if (status.state == 'proposed') {
          lastMessage = '7인치 화면에서 승인하면 실행됩니다.';
          notifyListeners();
        }
        if (status.finished) {
          lastMessage = switch (status.state) {
            'completed' => '장치가 응답했습니다.',
            // task-policy.json proposal_ttl_seconds: nobody approved it in time
            _ when status.failureCode == 'PROPOSAL_EXPIRED' =>
              '승인 시간이 지나 실행하지 않았습니다.',
            _ => '실행되지 않았습니다 (${status.failureCode ?? status.state}).',
          };
          await refresh(silent: true);
          return;
        }
      } on HomeMindUnauthorizedException catch (error) {
        await _unauthorized(error);
        return;
      } catch (_) {
        // keep polling until the deadline; refresh() reports connection errors
      }
    }
    await refresh(silent: true);
  }

  void _useCredential(PairingCredential stored) {
    credential = stored;
    _api.baseUrl = stored.baseUrl;
    _api.setTlsPin(stored.tlsSha256);
    _api.setToken(stored.token, forBaseUrl: stored.baseUrl);
  }

  Future<void> _unauthorized(HomeMindUnauthorizedException error) async {
    if (error.tokenSent) {
      // The server no longer accepts this phone's token (revoked or reset).
      await _credentials.clear();
      credential = null;
      _api.setToken(null);
      _api.setTlsPin(null);
    }
    _toPairing(error.message);
  }

  void _toPairing(String? message) {
    _refreshTimer?.cancel();
    _refreshTimer = null;
    stopLiveEvents();
    _approvals = const [];
    approvalAlert = null;
    needsPairing = true;
    usingDemoData = true;
    snapshot = HardwareDemoData.snapshot;
    selectedIndex = 0;
    lastError = message;
    notifyListeners();
  }

  static String _defaultPhoneName() {
    final platform = kIsWeb ? 'web' : Platform.operatingSystem;
    return 'HomeMind 앱 ($platform)';
  }

  void _replaceDevice(HardwareDevice updated) {
    snapshot = snapshot.copyWith(
      devices: snapshot.devices
          .map((item) => item.id == updated.id ? updated : item)
          .toList(),
    );
  }

  HardwareDevice _simulateDeviceCommand(
    HardwareDevice device,
    String command,
  ) => device.copyWith(
    online: true,
    state: switch (command) {
      'OPEN' => 'OPEN',
      'CLOSE' => 'CLOSED',
      'ON' => 'ON',
      'OFF' => 'OFF',
      _ => device.state,
    },
  );

  @override
  void dispose() {
    _refreshTimer?.cancel();
    stopLiveEvents();
    _api.close();
    super.dispose();
  }
}
