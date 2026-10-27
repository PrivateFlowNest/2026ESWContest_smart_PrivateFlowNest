import 'launch_environment_stub.dart'
    if (dart.library.io) 'launch_environment_io.dart' as platform;

/// `HOMEMIND_*` variables of the native process; empty on web.
Map<String, String> launchEnvironment() => platform.launchEnvironment();
