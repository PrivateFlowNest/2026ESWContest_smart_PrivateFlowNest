/// Web: no password files (web builds connect anonymously).
String readPasswordFileHead(String path) =>
    throw UnsupportedError('MQTT password files are not read on this platform');
