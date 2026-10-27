import 'dart:typed_data';

import 'camera_preview_stub.dart'
    if (dart.library.io) 'camera_preview_io.dart' as platform;

/// Live camera preview from the vision service on NODE-RPI5-02 over the home
/// router (user 2026-10-25: raw video may travel between the home nodes,
/// never to an outside server). The service answers this UI's address only,
/// keeps only the newest JPEG of each camera, stores nothing and serves
/// nothing while the privacy mode blocks cameras.
const kPreviewCameras = [
  (id: 'CAM-INDOOR-01', name: '방 카메라'),
  (id: 'CAM-OUTDOOR-01', name: '현관 카메라'),
];

class CameraPreviewStatus {
  const CameraPreviewStatus(
      {required this.cameraAllowed, required this.detecting});

  final bool cameraAllowed;

  /// Per camera: true once its empty scene exists and it judges presence.
  final Map<String, bool> detecting;

  /// `GET /preview/status`; null when anything is missing or malformed.
  static CameraPreviewStatus? fromJson(Object? data) {
    if (data is! Map) return null;
    final allowed = data['camera_allowed'];
    final cameras = data['cameras'];
    if (allowed is! bool || cameras is! List) return null;
    final detecting = <String, bool>{};
    for (final camera in cameras) {
      if (camera is! Map) return null;
      final id = camera['camera_id'];
      final judging = camera['detecting'];
      if (id is! String || judging is! bool) return null;
      detecting[id] = judging;
    }
    return CameraPreviewStatus(cameraAllowed: allowed, detecting: detecting);
  }
}

class CameraPreviewException implements Exception {
  const CameraPreviewException(this.message);

  final String message;

  @override
  String toString() => message;
}

/// Korean text for an HTTP status of the preview service.
String cameraPreviewMessage(int statusCode) => switch (statusCode) {
      403 => '이 화면의 주소는 영상을 볼 수 없게 설정돼 있어요 (Pi5-02 allowed_clients).',
      404 => '아직 이 카메라의 영상이 없어요.',
      423 => '사생활 보호 모드라 카메라가 꺼져 있어요.',
      503 => '카메라가 새 영상을 보내지 않아요.',
      _ => 'Pi5-02 비전 서비스가 영상을 주지 못했어요 ($statusCode).',
    };

abstract class CameraPreviewClient {
  /// Newest JPEG of [cameraId]. Throws [CameraPreviewException] (Korean).
  Future<Uint8List> frame(String cameraId);

  Future<CameraPreviewStatus> status();

  void close();
}

/// HTTP client for [baseUrl] (`http://10.10.10.4:8091`); null without a URL
/// or without dart:io (web build).
CameraPreviewClient? cameraPreviewClientFor(String? baseUrl) =>
    baseUrl == null ? null : platform.httpCameraPreviewClient(baseUrl);
