import 'dart:convert';

class PairingPayloadException implements Exception {
  const PairingPayloadException(this.message);

  final String message;

  @override
  String toString() => message;
}

class PairingPayload {
  const PairingPayload({
    required this.version,
    required this.deviceId,
    required this.deviceName,
    required this.baseUri,
    required this.pairingCode,
    required this.nonce,
    required this.expiresAt,
    this.tlsSha256,
  });

  factory PairingPayload.parse(String raw, {DateTime? now}) {
    final Object? decoded;
    try {
      decoded = jsonDecode(raw.trim());
    } on FormatException {
      throw const PairingPayloadException('QR 코드가 JSON 형식이 아닙니다.');
    }

    if (decoded is! Map) {
      throw const PairingPayloadException('HomeMind 페어링 QR이 아닙니다.');
    }
    final json = decoded.cast<String, dynamic>();
    final version = json['version'];
    if (version is! int || version != 1) {
      throw const PairingPayloadException('지원하지 않는 QR 버전입니다.');
    }

    final deviceId = _requiredString(json, 'device_id', '기기 ID');
    final deviceName = _requiredString(json, 'device_name', '기기 이름');
    final baseUrl = _requiredString(json, 'base_url', 'Pi 주소');
    final pairingCode = _requiredString(json, 'pairing_code', '페어링 코드');
    final nonce = _requiredString(json, 'nonce', '보안 nonce');
    final expiresAtText = _requiredString(json, 'expires_at', '만료 시간');

    final baseUri = Uri.tryParse(baseUrl);
    if (baseUri == null ||
        !baseUri.hasAuthority ||
        !{'http', 'https'}.contains(baseUri.scheme)) {
      throw const PairingPayloadException('Pi 주소가 올바르지 않습니다.');
    }

    // HTTPS (Mobile API 0.5): SHA-256 of the device's self-signed certificate;
    // the app then trusts exactly that certificate.
    final tlsValue = json['tls_sha256'];
    String? tlsSha256;
    if (tlsValue != null) {
      if (tlsValue is! String ||
          !RegExp(r'^[0-9a-fA-F]{64}$').hasMatch(tlsValue)) {
        throw const PairingPayloadException('보안 인증서 정보가 올바르지 않습니다.');
      }
      tlsSha256 = tlsValue.toLowerCase();
    }

    final expiresAt = DateTime.tryParse(expiresAtText)?.toUtc();
    if (expiresAt == null) {
      throw const PairingPayloadException('QR 만료 시간을 읽을 수 없습니다.');
    }
    if (!expiresAt.isAfter((now ?? DateTime.now()).toUtc())) {
      throw const PairingPayloadException('이 페어링 QR은 만료되었습니다.');
    }

    return PairingPayload(
      version: version,
      deviceId: deviceId,
      deviceName: deviceName,
      baseUri: baseUri,
      pairingCode: pairingCode,
      nonce: nonce,
      expiresAt: expiresAt,
      tlsSha256: baseUri.scheme == 'https' ? tlsSha256 : null,
    );
  }

  final int version;
  final String deviceId;
  final String deviceName;
  final Uri baseUri;
  final String pairingCode;
  final String nonce;
  final DateTime expiresAt;

  /// Certificate fingerprint for an https:// [baseUri]; null = the phone's
  /// usual certificate checks.
  final String? tlsSha256;

  String get baseUrl {
    final value = baseUri.toString();
    return value.endsWith('/') ? value.substring(0, value.length - 1) : value;
  }

  bool isExpiredAt(DateTime time) => !expiresAt.isAfter(time.toUtc());

  static String _requiredString(
    Map<String, dynamic> json,
    String key,
    String label,
  ) {
    final value = json[key];
    if (value is! String || value.trim().isEmpty) {
      throw PairingPayloadException('$label 정보가 없습니다.');
    }
    return value.trim();
  }
}
