/// Android's platform glue, over the app's own method channel
/// (`android/app/src/main/kotlin/.../Platform.kt`): the identity's seed
/// sealed by a Keystore key, the listening service, the runtime
/// permissions, and the phone's name. No plugin: a plugin for secure
/// storage would bring libsecret into the Linux build.
library;

import 'package:flutter/services.dart';

import 'background.dart';
import 'identity.dart';
import 'permissions.dart';

class AndroidPlatform implements KeyVault, BackgroundService, PermissionPlatform {
  AndroidPlatform({this.profile = 'default'});

  final String profile;
  static const _channel = MethodChannel('org.thistleos.thistle_chat/platform');

  @override
  Future<String?> read() => _channel.invokeMethod<String>('vault.read', {'profile': profile});

  @override
  Future<void> write(String seedHex) =>
      _channel.invokeMethod<void>('vault.write', {'profile': profile, 'seed': seedHex});

  @override
  Future<void> setRunning(bool on) => _channel.invokeMethod<void>('service.set', {'on': on});

  @override
  Future<PermissionAnswer> status(AppPermission p) => _answer('permission.status', p);

  @override
  Future<PermissionAnswer> request(AppPermission p) => _answer('permission.request', p);

  Future<PermissionAnswer> _answer(String method, AppPermission p) async {
    final a = await _channel.invokeMethod<String>(method, {'name': p.name});
    return PermissionAnswer.values.firstWhere((v) => v.name == a, orElse: () => PermissionAnswer.denied);
  }

  @override
  Future<void> openSettings() => _channel.invokeMethod<void>('settings.open');

  /// The phone's name as its owner set it, else its model.
  Future<String> deviceName() async => await _channel.invokeMethod<String>('device.name') ?? 'Android';
}
