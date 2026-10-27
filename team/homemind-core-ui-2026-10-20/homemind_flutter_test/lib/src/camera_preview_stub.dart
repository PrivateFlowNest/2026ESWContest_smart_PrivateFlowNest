import 'camera_preview.dart';

/// Web build: no camera preview (the browser would need the preview service to allow it).
CameraPreviewClient? httpCameraPreviewClient(String baseUrl) => null;
