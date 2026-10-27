import 'package:dbus/dbus.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:homemind_flutter_test/src/atlas_app_client.dart';

DBusMethodCall _event(String event) => DBusMethodCall(
    sender: ':1.42',
    interface: atlasClientInterface,
    name: 'SendEvent',
    values: [DBusString(event), DBusArray.string(const [])]);

void main() {
  test('Atlas client object: Status property and pause/restart events', () async {
    final client = AtlasAppClient();
    expect(client.path, DBusObjectPath(atlasClientPath));
    expect(client.status, '');

    expect(await client.handleMethodCall(_event('pause')),
        isA<DBusMethodSuccessResponse>());
    expect(client.status, 'paused');
    final status = await client.getProperty(atlasClientInterface, 'Status');
    expect(status.returnValues, [DBusVariant(DBusString('paused'))]);

    await client.handleMethodCall(_event('restart'));
    expect(client.status, 'running');
    await client.handleMethodCall(_event('reclaim'));
    expect(client.status, 'running');
  });

  test('Atlas client object refuses other calls', () async {
    final client = AtlasAppClient();
    expect(
        await client.handleMethodCall(DBusMethodCall(
            sender: ':1.42', interface: atlasClientInterface, name: 'Other')),
        isA<DBusMethodErrorResponse>());
    expect(
        await client.handleMethodCall(DBusMethodCall(
            sender: ':1.42',
            interface: atlasClientInterface,
            name: 'SendEvent',
            values: [DBusString('pause')])),
        isA<DBusMethodErrorResponse>());
    expect(client.status, '');
    expect(await client.getProperty(atlasClientInterface, 'Other'),
        isA<DBusMethodErrorResponse>());
    final xml = client.introspect().single;
    expect(xml.name, atlasClientInterface);
    expect(xml.methods.single.name, 'SendEvent');
    expect(xml.properties.single.name, 'Status');
  });

  test('no APP_ID (PC, systemd launcher): registration does nothing', () async {
    await registerWithAtlasAppManager();
  });
}
