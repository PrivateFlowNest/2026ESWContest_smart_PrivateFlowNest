import 'atlas_registration_stub.dart'
    if (dart.library.io) 'atlas_app_client.dart' as platform;

/// Atlas app manager handshake (native builds started as an Atlas app);
/// nothing on web.
Future<void> registerWithAtlasAppManager() =>
    platform.registerWithAtlasAppManager();
