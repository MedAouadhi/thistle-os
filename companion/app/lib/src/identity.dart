/// The companion's identity key: made on first start, its seed kept in the
/// platform's secure storage (spec, The companion app: Identity). On Linux
/// that is the Secret Service (GNOME Keyring, KWallet), spoken to directly
/// over D-Bus, so the build needs no libsecret. Android (ticket 11) adds a
/// Keystore-backed [KeyVault]; macOS keeps it in the login keychain
/// ([KeychainVault]).
library;

import 'dart:async';
import 'dart:convert';

import 'package:dbus/dbus.dart';
import 'package:flutter/services.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

/// Where the identity's 32-byte seed is kept, as 64 lowercase hex digits.
abstract interface class KeyVault {
  /// The seed, or null if none is kept.
  Future<String?> read();
  Future<void> write(String seedHex);
}

/// The identity whose seed is in `vault`, made and kept there on first
/// start. Something other than a seed in the vault is an error: replacing
/// it would change who this app is to every contact.
Future<Identity> loadIdentity(KeyVault vault) async {
  final kept = await vault.read();
  if (kept != null) {
    final hex = kept.trim();
    if (!isLowerHex(hex, 64)) throw const FormatException('the kept identity is not a 64-digit hex seed');
    return Identity.fromSeed(fromHex(hex));
  }
  final id = Identity.generate();
  await vault.write(toHex(id.seed));
  return id;
}

const _service = 'org.freedesktop.secrets';
const _ifService = 'org.freedesktop.Secret.Service';
const _ifItem = 'org.freedesktop.Secret.Item';
const _ifCollection = 'org.freedesktop.Secret.Collection';
const _ifPrompt = 'org.freedesktop.Secret.Prompt';
final _root = DBusObjectPath('/org/freedesktop/secrets');
final _defaultCollection = DBusObjectPath('/org/freedesktop/secrets/aliases/default');

/// The seed as an item of the Secret Service's default collection, found by
/// the attributes `application` and `profile` (a second instance on the
/// same machine runs under another profile). The session is "plain": the
/// secret crosses the user's own session bus only.
class SecretServiceVault implements KeyVault {
  SecretServiceVault({this.profile = 'default', DBusClient? client})
    : _client = client ?? DBusClient.session(),
      _ownsClient = client == null;

  final String profile;
  final DBusClient _client;
  final bool _ownsClient;
  DBusObjectPath? _session;

  Map<String, String> get _attributes => {'application': 'org.thistleos.thistle_chat', 'profile': profile};

  DBusRemoteObject _object(DBusObjectPath path) => DBusRemoteObject(_client, name: _service, path: path);

  Future<DBusObjectPath> _openSession() async {
    if (_session != null) return _session!;
    final r = await _object(
      _root,
    ).callMethod(_ifService, 'OpenSession', [const DBusString('plain'), const DBusVariant(DBusString(''))]);
    return _session = r.returnValues[1].asObjectPath();
  }

  /// Run a prompt the service asked for (an unlock dialog) to its end.
  Future<void> _prompt(DBusObjectPath prompt) async {
    if (prompt.value == '/') return;
    final object = _object(prompt);
    final done = DBusRemoteObjectSignalStream(object: object, interface: _ifPrompt, name: 'Completed').first;
    await object.callMethod(_ifPrompt, 'Prompt', [const DBusString('')]);
    final signal = await done;
    if (signal.values.first.asBoolean()) throw StateError('the keyring prompt was dismissed');
  }

  Future<void> _unlock(List<DBusObjectPath> paths) async {
    if (paths.isEmpty) return;
    final r = await _object(_root).callMethod(_ifService, 'Unlock', [DBusArray.objectPath(paths)]);
    await _prompt(r.returnValues[1].asObjectPath());
  }

  Future<DBusObjectPath?> _find() async {
    final r = await _object(_root).callMethod(_ifService, 'SearchItems', [_attrs()]);
    final unlocked = r.returnValues[0].asObjectPathArray().toList();
    final locked = r.returnValues[1].asObjectPathArray().toList();
    if (unlocked.isNotEmpty) return unlocked.first;
    if (locked.isEmpty) return null;
    await _unlock([locked.first]);
    return locked.first;
  }

  DBusDict _attrs() => DBusDict(DBusSignature('s'), DBusSignature('s'), {
    for (final e in _attributes.entries) DBusString(e.key): DBusString(e.value),
  });

  @override
  Future<String?> read() async {
    final item = await _find();
    if (item == null) return null;
    final session = await _openSession();
    final r = await _object(item).callMethod(_ifItem, 'GetSecret', [session]);
    final secret = r.returnValues[0].asStruct();
    return utf8.decode(secret[2].asByteArray().toList());
  }

  @override
  Future<void> write(String seedHex) async {
    final session = await _openSession();
    await _unlock([_defaultCollection]);
    final properties = DBusDict.stringVariant({
      '$_ifItem.Label': DBusString('Thistle Chat identity ($profile)'),
      '$_ifItem.Attributes': _attrs(),
    });
    final secret = DBusStruct([
      session,
      DBusArray.byte(const []),
      DBusArray.byte(utf8.encode(seedHex)),
      const DBusString('text/plain'),
    ]);
    final r = await _object(
      _defaultCollection,
    ).callMethod(_ifCollection, 'CreateItem', [properties, secret, const DBusBoolean(true)]);
    await _prompt(r.returnValues[1].asObjectPath());
  }

  /// Forget the identity (for tests, and a "new identity" later).
  Future<void> delete() async {
    final item = await _find();
    if (item == null) return;
    final r = await _object(item).callMethod(_ifItem, 'Delete', []);
    await _prompt(r.returnValues[0].asObjectPath());
  }

  Future<void> close() async {
    if (_ownsClient) await _client.close();
  }
}

/// The seed in the macOS login keychain, a generic password of service
/// `org.thistleos.thistle_chat` and account the profile, over the app's
/// own channel (`macos/Runner/MainFlutterWindow.swift`), so no plugin.
class KeychainVault implements KeyVault {
  KeychainVault({this.profile = 'default'});
  final String profile;
  static const _channel = MethodChannel('org.thistleos.thistle_chat/keychain');

  @override
  Future<String?> read() => _channel.invokeMethod<String>('read', {'profile': profile});

  @override
  Future<void> write(String seedHex) => _channel.invokeMethod<void>('write', {'profile': profile, 'seed': seedHex});

  /// Forget the identity.
  Future<void> delete() => _channel.invokeMethod<void>('delete', {'profile': profile});
}
