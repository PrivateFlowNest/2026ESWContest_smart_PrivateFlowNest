import 'package:flutter/material.dart';
import 'package:qr/qr.dart';

/// Phone pairing QR on the 7-inch screen (NODE-RPI5-01) for the phone app.
/// Contract: `contracts/pfn.v1/mobile-pairing-request.schema.json` and
/// `mobile-pairing-offer.schema.json` (QoS 1, not retained, core never
/// subscribes; the broker ACL lets only NODE-RPI5-01 read offers). The
/// one-time code is shown on screen only: never logged, never stored.
const kPairingRequestTopic = 'pfn/v1/mobile/pairing/request';
const kPairingOfferTopic = 'pfn/v1/mobile/pairing/offer';

/// The Mobile API ignores a request older than this (no replay after a reconnect).
const kPairingRequestLifetime = Duration(seconds: 5);

/// How long the screen waits for an offer before saying the Mobile API is not answering.
const kPairingReplyTimeout = Duration(seconds: 6);

final _code = RegExp(r'^[0-9A-Z]{4}-[0-9A-Z]{4}$');

class PhonePairingOffer {
  const PhonePairingOffer({
    required this.qrPayload,
    required this.pairingCode,
    required this.deviceName,
    required this.baseUrl,
    required this.expiresAt,
    this.simulated = false,
  });

  /// Exact text to draw as the QR (what `--print-pairing-qr --json` prints on the device).
  final String qrPayload;
  final String pairingCode;
  final String deviceName;
  final String baseUrl;
  final DateTime expiresAt;
  final bool simulated;

  /// Offer fields of a `status: offered` message; null when anything is missing or malformed.
  static PhonePairingOffer? fromJson(Map<String, dynamic> data) {
    final payload = data['qr_payload'];
    final code = data['pairing_code'];
    final name = data['device_name'];
    final url = data['base_url'];
    final expires = data['expires_at_ms'];
    if (data['status'] != 'offered' ||
        payload is! String ||
        payload.length < 2 ||
        payload.length > 1024 ||
        code is! String ||
        !_code.hasMatch(code) ||
        name is! String ||
        url is! String ||
        !(url.startsWith('http://') || url.startsWith('https://')) ||
        expires is! int) {
      return null;
    }
    return PhonePairingOffer(
      qrPayload: payload,
      pairingCode: code,
      deviceName: name,
      baseUrl: url,
      expiresAt: DateTime.fromMillisecondsSinceEpoch(expires),
      simulated: data['simulated'] == true,
    );
  }
}

class PhonePairingException implements Exception {
  const PhonePairingException(this.message);

  final String message;

  @override
  String toString() => message;
}

Map<String, dynamic> buildPairingRequestPayload({
  required String clientId,
  required String eventId,
  required String requestId,
  required int sentAtMs,
}) =>
    {
      'schema_version': 'pfn.v1',
      'event_id': eventId,
      'request_id': requestId,
      'source_node_id': 'NODE-RPI5-01',
      'target_node_id': 'NODE-RPI4-01',
      'client_id': clientId,
      'sent_at_ms': sentAtMs,
      'expires_at_ms': sentAtMs + kPairingRequestLifetime.inMilliseconds,
    };

/// Korean text for a `status: unavailable` reason from the Mobile API.
String pairingUnavailableMessage(String? reason) => switch (reason) {
      'public_base_url is not set on the Mobile API' =>
        'Pi4 Mobile API에 휴대폰이 접속할 주소(public_base_url)가 설정되지 않았어요.',
      'too many requests, try again' => '방금 QR을 만들었어요. 잠시 후 다시 눌러 주세요.',
      _ => '지금은 QR을 만들 수 없어요. 잠시 후 다시 시도해 주세요.',
    };

/// Paired phones on the 7-inch UI (contracts/pfn.v1 mobile-phones-request / mobile-phones-state, 2026-10-25):
/// the Mobile API lists the phones that hold a valid token and unpairs one on request. Token ids only, never tokens;
/// the broker ACL lets only NODE-RPI5-01 read the list.
const kPhonesRequestTopic = 'pfn/v1/mobile/phones/request';
const kPhonesStateTopic = 'pfn/v1/mobile/phones/state';

final _phoneId = RegExp(r'^[0-9a-f]{8}$');

class PairedPhone {
  const PairedPhone({
    required this.phoneId,
    required this.deviceName,
    required this.pairedAt,
    required this.lastSeenAt,
  });

  /// Token id (8 hex digits), what an unpair request names.
  final String phoneId;
  final String deviceName;

  /// Null when the Mobile API does not know it.
  final DateTime? pairedAt;

  /// The phone's last accepted request since the Mobile API started; null when none.
  final DateTime? lastSeenAt;
}

class PairedPhones {
  const PairedPhones({required this.phones, this.revoked, this.simulated = false});

  final List<PairedPhone> phones;

  /// For an unpair answer: whether a phone was removed (false: it was not paired any more).
  final bool? revoked;
  final bool simulated;

  /// The phones of a `status: ok` state; null when anything is missing or malformed.
  static PairedPhones? fromJson(Map<String, dynamic> data) {
    final items = data['phones'];
    if (data['status'] != 'ok' || items is! List || items.length > 50) return null;
    final phones = <PairedPhone>[];
    for (final item in items) {
      if (item is! Map) return null;
      final id = item['phone_id'];
      final name = item['device_name'];
      final paired = item['paired_at'];
      final seen = item['last_seen_at_ms'];
      if (id is! String ||
          !_phoneId.hasMatch(id) ||
          name is! String ||
          name.isEmpty ||
          name.length > 64 ||
          paired is! String ||
          (seen != null && seen is! int)) {
        return null;
      }
      phones.add(PairedPhone(
        phoneId: id,
        deviceName: name,
        pairedAt: DateTime.tryParse(paired)?.toLocal(),
        lastSeenAt: seen is int ? DateTime.fromMillisecondsSinceEpoch(seen) : null,
      ));
    }
    final revoked = data['revoked'];
    return PairedPhones(
      phones: List.unmodifiable(phones),
      revoked: revoked is bool ? revoked : null,
      simulated: data['simulated'] == true,
    );
  }
}

/// [revokePhoneId] null: list the phones; otherwise unpair that phone.
Map<String, dynamic> buildPhonesRequestPayload({
  required String clientId,
  required String eventId,
  required String requestId,
  required int sentAtMs,
  String? revokePhoneId,
}) =>
    {
      'schema_version': 'pfn.v1',
      'event_id': eventId,
      'request_id': requestId,
      'source_node_id': 'NODE-RPI5-01',
      'target_node_id': 'NODE-RPI4-01',
      'client_id': clientId,
      'action': revokePhoneId == null ? 'list' : 'revoke',
      if (revokePhoneId != null) 'phone_id': revokePhoneId,
      'sent_at_ms': sentAtMs,
      'expires_at_ms': sentAtMs + kPairingRequestLifetime.inMilliseconds,
    };

/// Korean text for a `status: unavailable` reason from the Mobile API.
String phonesUnavailableMessage(String? reason) => switch (reason) {
      'the phone could not be unpaired on the Mobile API' => 'Pi4에서 연결을 끊지 못했어요. 잠시 후 다시 시도해 주세요.',
      _ => 'Pi4에서 연결된 휴대폰 목록을 읽지 못했어요.',
    };

/// Draws [data] as a QR code (error correction M, 4-module quiet zone) in black on white.
class PairingQrCode extends StatelessWidget {
  const PairingQrCode({super.key, required this.data, this.size = 300});

  final String data;
  final double size;

  @override
  Widget build(BuildContext context) {
    final image = QrImage(
        QrCode.fromData(data: data, errorCorrectLevel: QrErrorCorrectLevel.M));
    return Semantics(
      label: '휴대폰 연결 QR 코드',
      child: CustomPaint(size: Size.square(size), painter: _QrPainter(image)),
    );
  }
}

class _QrPainter extends CustomPainter {
  _QrPainter(this.image);

  final QrImage image;

  @override
  void paint(Canvas canvas, Size size) {
    const quiet = 4;
    final count = image.moduleCount + 2 * quiet;
    final module = size.shortestSide / count;
    canvas.drawRect(Offset.zero & size, Paint()..color = Colors.white);
    final dark = Paint()
      ..color = Colors.black
      ..isAntiAlias = false;
    for (var row = 0; row < image.moduleCount; row++) {
      for (var col = 0; col < image.moduleCount; col++) {
        if (!image.isDark(row, col)) continue;
        canvas.drawRect(
            Rect.fromLTWH((col + quiet) * module, (row + quiet) * module,
                module + 0.3, module + 0.3),
            dark);
      }
    }
  }

  @override
  bool shouldRepaint(_QrPainter oldDelegate) => oldDelegate.image != image;
}
