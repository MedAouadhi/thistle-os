import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/identity.dart';

class MemoryVault implements KeyVault {
  String? seed;
  int writes = 0;
  @override
  Future<String?> read() async => seed;
  @override
  Future<void> write(String seedHex) async {
    writes++;
    seed = seedHex;
  }
}

void main() {
  test('the identity is made on first start and kept in the vault', () async {
    final vault = MemoryVault();
    final first = await loadIdentity(vault);
    expect(vault.seed, matches(RegExp(r'^[0-9a-f]{64}$')));
    expect(vault.writes, 1);

    // A restart: the same vault gives the same key, and nothing is written.
    final again = await loadIdentity(vault);
    expect(again.keyHex, first.keyHex);
    expect(vault.writes, 1);
  });

  test('a vault holding something else is refused, not overwritten', () async {
    final vault = MemoryVault()..seed = 'not a seed';
    await expectLater(loadIdentity(vault), throwsFormatException);
    expect(vault.seed, 'not a seed');
  });

  // The real Secret Service, when this session has one (a desktop; not CI).
  final bus = Platform.environment['DBUS_SESSION_BUS_ADDRESS'];
  test('the Secret Service keeps the seed across a reconnect, and forgets it on delete', () async {
    final profile = 'test-${DateTime.now().microsecondsSinceEpoch}';
    final vault = SecretServiceVault(profile: profile);
    try {
      expect(await vault.read(), isNull);
      final id = await loadIdentity(vault);
      await vault.close();

      final reopened = SecretServiceVault(profile: profile);
      expect((await loadIdentity(reopened)).keyHex, id.keyHex);
      await reopened.delete();
      expect(await reopened.read(), isNull);
      await reopened.close();
    } finally {
      await vault.close();
    }
  }, skip: bus == null || Platform.environment['CI'] != null ? 'no desktop session bus' : false);
}
