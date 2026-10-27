import 'dart:async';
import 'dart:convert';
import 'dart:io';

import '../../models/approval_models.dart';
import '../../models/hardware_models.dart';
import '../../models/pairing_models.dart';
import 'sha256.dart';

class HomeMindApiException implements Exception {
  const HomeMindApiException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// 401 from the Mobile API: no or revoked pairing token. [tokenSent] tells whether
/// this phone's stored token was part of the request (then it is no longer valid).
class HomeMindUnauthorizedException extends HomeMindApiException {
  const HomeMindUnauthorizedException({required this.tokenSent})
    : super('이 휴대폰의 연결이 해제되었습니다. 기기의 QR로 다시 연결하세요.');

  final bool tokenSent;
}

/// POST /api/v1/pairing/claim failed (expired/used code, too many attempts, ...).
class HomeMindPairingException extends HomeMindApiException {
  const HomeMindPairingException(super.message);
}

/// Result of POST /api/v1/devices/{id}/commands (Mobile API 0.3+): the request went
/// to core as a task; the device state changes only after core completes it.
class DeviceCommandResult {
  const DeviceCommandResult({
    required this.device,
    required this.message,
    this.taskId,
    this.taskState,
  });

  final HardwareDevice device;
  final String message;
  final String? taskId;
  final String? taskState;
}

class TaskStatus {
  const TaskStatus({required this.state, this.failureCode});

  final String state;
  final String? failureCode;

  bool get finished =>
      const {'completed', 'failed', 'rejected', 'cancelled'}.contains(state);
}

class PairingClaimResult {
  const PairingClaimResult({
    required this.token,
    required this.tokenId,
    required this.deviceId,
    required this.deviceName,
  });

  final String token;
  final String tokenId;
  final String deviceId;
  final String deviceName;
}

/// Whether [der] (a server certificate) is the one the pairing QR named.
bool certificateMatchesPin(List<int> der, String? pin) =>
    pin != null && sha256Hex(der) == pin.toLowerCase();

class HomeMindApi {
  HomeMindApi({String baseUrl = 'http://10.0.2.2:8000'})
    : _baseUrl = _normalize(baseUrl),
      _client = _newClient(null);

  HttpClient _client;
  String _baseUrl;
  String? _token;
  String? _tokenBaseUrl;
  String? _tlsPin;

  String get baseUrl => _baseUrl;

  /// HTTPS with the device's self-signed certificate: only the certificate
  /// whose SHA-256 is [sha256] (from the pairing QR) is accepted, no
  /// certificate authority is consulted. null = the platform's usual checks.
  void setTlsPin(String? sha256) {
    final pin = sha256?.toLowerCase();
    if (pin == _tlsPin) return;
    _tlsPin = pin;
    final old = _client;
    _client = _newClient(pin);
    old.close();
  }

  static HttpClient _newClient(String? pin) {
    final client = pin == null
        ? HttpClient()
        : (HttpClient(context: SecurityContext(withTrustedRoots: false))
            ..badCertificateCallback = (certificate, host, port) =>
                certificateMatchesPin(certificate.der, pin));
    return client..connectionTimeout = const Duration(seconds: 4);
  }

  set baseUrl(String value) => _baseUrl = _normalize(value);

  /// Whether requests to the current [baseUrl] carry the pairing token.
  bool get hasToken => _token != null && _tokenBaseUrl == _baseUrl;

  /// The bearer token is bound to the server it was issued by: it is never
  /// sent after [baseUrl] moves to another address.
  void setToken(String? token, {String? forBaseUrl}) {
    _token = token;
    _tokenBaseUrl = token == null ? null : _normalize(forBaseUrl ?? _baseUrl);
  }

  static String _normalize(String value) {
    final trimmed = value.trim();
    return trimmed.endsWith('/')
        ? trimmed.substring(0, trimmed.length - 1)
        : trimmed;
  }

  Future<bool> healthCheck() async {
    final data = await _request('/api/v1/health', authenticated: false);
    return data['status'] == 'ok';
  }

  Future<HardwareSnapshot> getDashboard() async {
    final data = await _request('/api/v1/dashboard');
    return HardwareSnapshot.fromJson(
      (data['snapshot'] as Map).cast<String, dynamic>(),
    );
  }

  Future<DeviceCommandResult> sendDeviceCommand(
    String deviceId,
    String command,
  ) async {
    final encodedId = Uri.encodeComponent(deviceId);
    final data = await _request(
      '/api/v1/devices/$encodedId/commands',
      method: 'POST',
      body: {'command': command},
    );
    return DeviceCommandResult(
      device: HardwareDevice.fromJson(
        (data['device'] as Map).cast<String, dynamic>(),
      ),
      message: data['message'] as String? ?? '',
      taskId: data['task_id'] as String?,
      taskState: data['task_state'] as String?,
    );
  }

  Future<TaskStatus> getTask(String taskId) async {
    final data = await _request('/api/v1/tasks/${Uri.encodeComponent(taskId)}');
    return TaskStatus(
      state: data['state'] as String? ?? 'unknown',
      failureCode: data['failure_code'] as String?,
    );
  }

  /// Tasks waiting for approval (AI proposals, scenarios, the entrance door).
  Future<List<ApprovalItem>> getApprovals() async {
    final data = await _request('/api/v1/approvals');
    return ((data['approvals'] as List?) ?? const [])
        .whereType<Map>()
        .map((item) => ApprovalItem.fromJson(item.cast<String, dynamic>()))
        .toList();
  }

  /// POST /api/v1/tasks/{id}/approve or /reject. Core's answer arrives as a
  /// task change (live events or [getTask]), not in this response.
  Future<void> decideTask(String taskId, {required bool approve}) async {
    try {
      await _request(
        '/api/v1/tasks/${Uri.encodeComponent(taskId)}/${approve ? 'approve' : 'reject'}',
        method: 'POST',
        describeErrors: false,
      );
    } on _HttpStatusException catch (error) {
      throw HomeMindApiException(switch (error.statusCode) {
        403 => '현관문 열기는 집 안 7인치 화면에서 승인해요.',
        404 => '이미 처리됐거나 승인 시간이 지난 요청입니다.',
        503 => '집 서버(core)와 연결이 끊겨 보내지 못했습니다.',
        _ => '보내지 못했습니다 (서버 오류 ${error.statusCode}).',
      });
    }
  }

  /// GET /api/v1/events (server-sent events) while the app runs; starts with an
  /// "open" event. [after] resumes after the last event seen. Not OS push: a
  /// closed app hears nothing. Ends with an error when nothing (not even the
  /// server's 15 s keep-alive) arrives for [silence]. Cancelling aborts the
  /// request at once (an open HTTP response would not end by itself).
  Stream<LiveEvent> liveEvents({
    int? after,
    Duration silence = const Duration(seconds: 40),
  }) {
    final uri = Uri.parse(
      '$_baseUrl/api/v1/events${after == null ? '' : '?after=$after'}',
    );
    final token = hasToken ? _token : null;
    HttpClientRequest? request;
    StreamSubscription<LiveEvent>? events;
    var cancelled = false;
    final controller = StreamController<LiveEvent>();

    Future<void> fail(Object error) async {
      if (cancelled) return;
      controller.addError(error);
      await controller.close();
    }

    controller.onListen = () async {
      if (token == null) {
        return fail(const HomeMindUnauthorizedException(tokenSent: false));
      }
      final HttpClientResponse response;
      try {
        request = await _client.getUrl(uri);
        request!.headers.set('Accept', 'text/event-stream');
        request!.headers.set(HttpHeaders.authorizationHeader, 'Bearer $token');
        response = await request!.close().timeout(const Duration(seconds: 6));
      } on TimeoutException {
        request?.abort();
        return fail(const HomeMindApiException('실시간 알림 연결 시간이 초과되었습니다.'));
      } on TlsException {
        return fail(const HomeMindApiException(_tlsFailed));
      } on SocketException {
        return fail(HomeMindApiException('$_baseUrl에 연결할 수 없습니다.'));
      } on HttpException catch (error) {
        return fail(HomeMindApiException('실시간 알림 연결이 끊겼습니다 (${error.message}).'));
      }
      if (cancelled) return;
      if (response.statusCode != 200) {
        request!.abort();
        return fail(
          response.statusCode == 401
              ? const HomeMindUnauthorizedException(tokenSent: true)
              : HomeMindApiException('실시간 알림 연결 실패 (${response.statusCode})'),
        );
      }
      controller.add(const LiveEvent('open', {}));
      events = parseServerSentEvents(
        response
            .timeout(silence)
            .cast<List<int>>()
            .transform(utf8.decoder)
            .transform(const LineSplitter()),
      ).listen(
        controller.add,
        onError: (Object error) => fail(error),
        onDone: () => cancelled ? null : controller.close(),
      );
    };
    controller.onCancel = () {
      cancelled = true;
      request?.abort();
      // not awaited: the aborted response ends on its own ("connection closed")
      events?.cancel().ignore();
    };
    return controller.stream;
  }

  /// Exchanges the one-time code of a scanned QR for a bearer token
  /// (POST {payload.baseUrl}/api/v1/pairing/claim). Sends no token itself.
  Future<PairingClaimResult> claimPairing(
    PairingPayload payload, {
    required String phoneName,
  }) async {
    setTlsPin(payload.tlsSha256);
    final Map<String, dynamic> data;
    try {
      data = await _request(
        '/api/v1/pairing/claim',
        method: 'POST',
        body: {
          'code': payload.pairingCode,
          'nonce': payload.nonce,
          'device_name': phoneName,
        },
        authenticated: false,
        baseUrlOverride: payload.baseUrl,
        describeErrors: false,
      );
    } on _HttpStatusException catch (error) {
      throw HomeMindPairingException(switch (error.statusCode) {
        401 => '코드가 맞지 않거나 만료되었거나 이미 사용되었습니다. 기기에서 새 QR을 만들어 다시 스캔하세요.',
        429 =>
          '실패한 연결 시도가 많아 잠시 막혔습니다.'
              '${error.retryAfter != null ? ' ${error.retryAfter}초 후' : ' 잠시 후'} 다시 시도하세요.',
        422 => 'QR 내용이 올바르지 않습니다. 기기에서 새 QR을 만들어 주세요.',
        _ => '연결하지 못했습니다 (서버 오류 ${error.statusCode}).',
      });
    }
    final token = data['token'];
    if (token is! String || token.isEmpty) {
      throw const HomeMindPairingException('서버가 토큰을 보내지 않았습니다.');
    }
    return PairingClaimResult(
      token: token,
      tokenId: data['token_id'] as String? ?? '',
      deviceId: data['device_id'] as String? ?? payload.deviceId,
      deviceName: data['device_name'] as String? ?? payload.deviceName,
    );
  }

  Future<Map<String, dynamic>> _request(
    String path, {
    String method = 'GET',
    Map<String, dynamic>? body,
    bool authenticated = true,
    String? baseUrlOverride,
    bool describeErrors = true,
  }) async {
    final base = baseUrlOverride == null
        ? _baseUrl
        : _normalize(baseUrlOverride);
    final sendToken =
        authenticated && _token != null && _tokenBaseUrl == base;
    try {
      final uri = Uri.parse('$base$path');
      final request = method == 'POST'
          ? await _client.postUrl(uri)
          : await _client.getUrl(uri);
      request.headers.contentType = ContentType.json;
      request.headers.set('Accept', 'application/json');
      if (sendToken) {
        request.headers.set(HttpHeaders.authorizationHeader, 'Bearer $_token');
      }
      if (body != null) {
        request.write(jsonEncode(body));
      }

      final response = await request.close().timeout(
        const Duration(seconds: 6),
      );
      final responseBody = await utf8.decoder.bind(response).join();
      if (response.statusCode == 401 && authenticated) {
        throw HomeMindUnauthorizedException(tokenSent: sendToken);
      }
      if (response.statusCode < 200 || response.statusCode >= 300) {
        if (!describeErrors) {
          throw _HttpStatusException(
            response.statusCode,
            int.tryParse(response.headers.value('retry-after') ?? ''),
          );
        }
        throw HomeMindApiException(
          '서버 오류 ${response.statusCode}: $responseBody',
        );
      }
      final decoded = jsonDecode(responseBody);
      if (decoded is! Map) {
        throw const HomeMindApiException('서버 응답 형식이 올바르지 않습니다.');
      }
      return decoded.cast<String, dynamic>();
    } on TimeoutException {
      throw const HomeMindApiException('Pi 응답 시간이 초과되었습니다.');
    } on TlsException {
      throw const HomeMindApiException(_tlsFailed);
    } on SocketException {
      throw HomeMindApiException('$base에 연결할 수 없습니다.');
    } on FormatException {
      throw const HomeMindApiException('서버의 JSON 응답을 읽지 못했습니다.');
    }
  }

  void close() => _client.close(force: true);
}

const _tlsFailed =
    '보안 연결(HTTPS)을 확인하지 못했습니다. 기기의 인증서가 바뀌었다면 새 QR로 다시 연결하세요.';

class _HttpStatusException implements Exception {
  const _HttpStatusException(this.statusCode, this.retryAfter);

  final int statusCode;
  final int? retryAfter;
}
