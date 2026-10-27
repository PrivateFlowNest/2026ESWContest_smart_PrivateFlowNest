import 'dart:convert';

import 'package:flutter_secure_storage/flutter_secure_storage.dart';

/// What the phone keeps after a successful QR pairing (POST /api/v1/pairing/claim).
/// The token is only ever sent to [baseUrl].
class PairingCredential {
  const PairingCredential({
    required this.baseUrl,
    required this.token,
    required this.tokenId,
    required this.deviceId,
    required this.deviceName,
    this.tlsSha256,
  });

  factory PairingCredential.fromJson(Map<String, dynamic> json) =>
      PairingCredential(
        baseUrl: json['base_url'] as String,
        token: json['token'] as String,
        tokenId: json['token_id'] as String? ?? '',
        deviceId: json['device_id'] as String? ?? '',
        deviceName: json['device_name'] as String? ?? '',
        tlsSha256: json['tls_sha256'] as String?,
      );

  final String baseUrl;
  final String token;
  final String tokenId;
  final String deviceId;
  final String deviceName;

  /// Pinned HTTPS certificate (pairing QR), null for plain http.
  final String? tlsSha256;

  Map<String, dynamic> toJson() => {
    'base_url': baseUrl,
    'token': token,
    'token_id': tokenId,
    'device_id': deviceId,
    'device_name': deviceName,
    if (tlsSha256 != null) 'tls_sha256': tlsSha256,
  };
}

abstract class CredentialStore {
  Future<PairingCredential?> read();
  Future<void> write(PairingCredential credential);
  Future<void> clear();
}

/// Android Keystore-backed storage (flutter_secure_storage: RSA OAEP + AES-GCM).
/// Auto backup is disabled in AndroidManifest.xml so the token never leaves the phone.
class SecureCredentialStore implements CredentialStore {
  SecureCredentialStore({FlutterSecureStorage? storage})
    : _storage = storage ?? const FlutterSecureStorage();

  static const _key = 'homemind.pairing.v1';
  final FlutterSecureStorage _storage;

  @override
  Future<PairingCredential?> read() async {
    final raw = await _storage.read(key: _key);
    if (raw == null) return null;
    try {
      final decoded = jsonDecode(raw);
      if (decoded is Map) {
        return PairingCredential.fromJson(decoded.cast<String, dynamic>());
      }
    } on Object {
      // unreadable entry: treated as not paired
    }
    await clear();
    return null;
  }

  @override
  Future<void> write(PairingCredential credential) =>
      _storage.write(key: _key, value: jsonEncode(credential.toJson()));

  @override
  Future<void> clear() => _storage.delete(key: _key);
}

/// In-memory store for tests and the demo.
class MemoryCredentialStore implements CredentialStore {
  MemoryCredentialStore([this.value]);

  PairingCredential? value;

  @override
  Future<PairingCredential?> read() async => value;

  @override
  Future<void> write(PairingCredential credential) async => value = credential;

  @override
  Future<void> clear() async => value = null;
}
