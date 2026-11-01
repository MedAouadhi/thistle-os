/// The headless core's identity key, as a file: the 32-byte seed in hex,
/// made on first start, readable by its owner alone. The Flutter app keeps
/// its seed in the platform's secure storage instead (ticket 06).
library;

import 'dart:io';

import 'protocol.dart';

/// The identity whose seed is in `path`, made and saved there if the file
/// does not exist.
Identity loadOrCreateIdentity(String path) {
  final file = File(path);
  if (file.existsSync()) {
    final hex = file.readAsStringSync().trim();
    if (!isLowerHex(hex, 64)) throw FormatException('$path does not hold a 64-digit lowercase hex seed');
    return Identity.fromSeed(fromHex(hex));
  }
  final id = Identity.generate();
  file.parent.createSync(recursive: true);
  file.createSync(exclusive: true);
  if (!Platform.isWindows) Process.runSync('chmod', ['600', path]);
  file.writeAsStringSync('${toHex(id.seed)}\n', flush: true);
  return id;
}
