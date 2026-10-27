import 'dart:convert';
import 'dart:io';

/// At most the first 4 KiB of [path]; only the first line is used.
String readPasswordFileHead(String path) {
  final file = File(path).openSync();
  try {
    return utf8.decode(file.readSync(4096), allowMalformed: true);
  } finally {
    file.closeSync();
  }
}
