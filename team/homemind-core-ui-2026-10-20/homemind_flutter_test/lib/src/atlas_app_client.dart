import 'dart:async';
import 'dart:io';

import 'package:dbus/dbus.dart';
import 'package:flutter/foundation.dart';

/// Atlas app manager handshake of the 7-inch UI.
///
/// When com.atlas.AppManager1 starts an app (environment APP_ID), the app has
/// about 3 s to call Register(app_id, events) on the system bus from its own
/// process, or the app manager stops it ("register timeout detected", real
/// Pi5-01, 2026-10-23). Atlas SDK apps do this in the AMClient library; this
/// build uses the open-source flutter-elinux, so the UI registers itself and
/// serves the same client object as Atlas apps: SendEvent(s, as) and the
/// Status property on /com/atlas/AppManagerClient1.
const atlasClientPath = '/com/atlas/AppManagerClient1';
const atlasClientInterface = 'com.atlas.AppManagerClient1';

/// Events the app manager may send through SendEvent.
const atlasClientEvents = ['pause', 'restart'];

class AtlasAppClient extends DBusObject {
  AtlasAppClient() : super(DBusObjectPath(atlasClientPath));

  /// Empty until the app manager sends an event, like the Atlas home app.
  String status = '';

  @override
  List<DBusIntrospectInterface> introspect() => [
        DBusIntrospectInterface(atlasClientInterface, methods: [
          DBusIntrospectMethod('SendEvent', args: [
            DBusIntrospectArgument(
                DBusSignature('s'), DBusArgumentDirection.in_,
                name: 'event'),
            DBusIntrospectArgument(
                DBusSignature('as'), DBusArgumentDirection.in_,
                name: 'params'),
          ]),
        ], properties: [
          DBusIntrospectProperty('Status', DBusSignature('s'),
              access: DBusPropertyAccess.read),
        ]),
      ];

  @override
  Future<DBusMethodResponse> handleMethodCall(DBusMethodCall methodCall) async {
    if (methodCall.interface != atlasClientInterface ||
        methodCall.name != 'SendEvent') {
      return DBusMethodErrorResponse.unknownMethod();
    }
    if (methodCall.signature != DBusSignature('sas')) {
      return DBusMethodErrorResponse.invalidArgs();
    }
    // Acknowledge like the AMClient samples: pause -> paused, restart -> running.
    switch (methodCall.values[0].asString()) {
      case 'pause':
        await _setStatus('paused');
      case 'restart':
        await _setStatus('running');
    }
    return DBusMethodSuccessResponse();
  }

  @override
  Future<DBusMethodResponse> getProperty(String interface, String name) async {
    if (interface == atlasClientInterface && name == 'Status') {
      return DBusGetPropertyResponse(DBusString(status));
    }
    return DBusMethodErrorResponse.unknownProperty();
  }

  @override
  Future<DBusMethodResponse> getAllProperties(String interface) async =>
      DBusGetAllPropertiesResponse(interface == atlasClientInterface
          ? {'Status': DBusString(status)}
          : {});

  Future<void> _setStatus(String value) async {
    status = value;
    if (client != null) {
      await emitPropertiesChanged(atlasClientInterface,
          changedProperties: {'Status': DBusString(value)});
    }
  }
}

/// Kept open for the app's lifetime: the app manager knows the app by this
/// connection.
DBusClient? _atlasBus;

/// Registers with the Atlas app manager when it started this process
/// (APP_ID set); does nothing otherwise (systemd launcher, PC, tests).
Future<void> registerWithAtlasAppManager() async {
  final appId = Platform.environment['APP_ID'] ?? '';
  if (appId.isEmpty || _atlasBus != null) return;
  final bus = DBusClient.system();
  try {
    await bus.registerObject(AtlasAppClient());
    await bus
        .callMethod(
          destination: 'com.atlas.AppManager1',
          path: DBusObjectPath('/com/atlas/AppManager1'),
          interface: 'com.atlas.AppManager1',
          name: 'Register',
          values: [DBusString(appId), DBusArray.string(atlasClientEvents)],
          replySignature: DBusSignature(''),
        )
        .timeout(const Duration(seconds: 2));
    _atlasBus = bus;
    debugPrint('homemind-ui: registered with the Atlas app manager as $appId');
  } catch (error) {
    debugPrint('homemind-ui: Atlas app manager registration failed: $error');
    await bus.close();
  }
}
