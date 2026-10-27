import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:typed_data';

import 'camera_preview.dart';

CameraPreviewClient? httpCameraPreviewClient(String baseUrl) =>
    _HttpCameraPreviewClient(baseUrl);

class _HttpCameraPreviewClient implements CameraPreviewClient {
  _HttpCameraPreviewClient(this.baseUrl)
      : _http = HttpClient()
          ..connectionTimeout = const Duration(seconds: 2)
          ..idleTimeout = const Duration(seconds: 10);

  final String baseUrl;
  final HttpClient _http;

  Future<(int, Uint8List)> _get(String path) async {
    const unreachable = CameraPreviewException(
        'Pi5-02 비전 서비스에 연결할 수 없어요. 비전 서비스가 꺼져 있을 수 있어요.');
    try {
      final request = await _http.getUrl(Uri.parse('$baseUrl$path'));
      final response =
          await request.close().timeout(const Duration(seconds: 3));
      final body = BytesBuilder(copy: false);
      await for (final chunk
          in response.timeout(const Duration(seconds: 3))) {
        body.add(chunk);
      }
      return (response.statusCode, body.takeBytes());
    } on TimeoutException {
      throw const CameraPreviewException('Pi5-02 비전 서비스가 응답하지 않아요.');
    } on SocketException {
      throw unreachable;
    } on HttpException {
      throw unreachable;
    }
  }

  @override
  Future<Uint8List> frame(String cameraId) async {
    final (code, body) = await _get('/preview/$cameraId.jpg');
    if (code != 200) throw CameraPreviewException(cameraPreviewMessage(code));
    return body;
  }

  @override
  Future<CameraPreviewStatus> status() async {
    final (code, body) = await _get('/preview/status');
    if (code != 200) throw CameraPreviewException(cameraPreviewMessage(code));
    final status = CameraPreviewStatus.fromJson(
        jsonDecode(utf8.decode(body, allowMalformed: true)));
    if (status == null) {
      throw const CameraPreviewException('Pi5-02가 보낸 카메라 상태가 올바르지 않아요.');
    }
    return status;
  }

  @override
  void close() => _http.close(force: true);
}
