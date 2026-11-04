/// Android's platform glue, over the app's own method channel
/// (`android/app/src/main/kotlin/.../Platform.kt`): the identity's seed
/// sealed by a Keystore key, the listening service, the runtime
/// permissions, the phone's name, and a call's ring and its window over
/// the lock screen (CallRing.kt, CallActivity.kt). No plugin: a plugin for
/// secure storage would bring libsecret into the Linux build.
library;

import 'package:flutter/foundation.dart';
import 'package:flutter/services.dart';

import 'background.dart';
import 'identity.dart';
import 'notifier.dart';
import 'permissions.dart';

class AndroidPlatform implements KeyVault, BackgroundService, PermissionPlatform, CallRinger {
  AndroidPlatform({this.profile = 'default'}) {
    _channel.setMethodCallHandler(_fromAndroid);
  }

  final String profile;
  static const _channel = MethodChannel('org.thistleos.thistle_chat/platform');

  /// The call notification's Answer (true) and Decline (false).
  void Function(String callId, bool answer)? onCall;

  /// The call window over the lock screen, as Android shows and hides it.
  late final AndroidCallWindow callWindow = AndroidCallWindow._();

  Future<void> _fromAndroid(MethodCall call) async {
    switch (call.method) {
      case 'call.window':
        callWindow._set(call.arguments['shown'] == true);
      case 'call.answer':
        onCall?.call(call.arguments['id'] as String, true);
      case 'call.decline':
        onCall?.call(call.arguments['id'] as String, false);
    }
  }

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

  @override
  Future<bool> canFillScreen() async => await _channel.invokeMethod<bool>('fillscreen.can') ?? true;

  @override
  Future<void> openFillScreenSettings() => _channel.invokeMethod<void>('fillscreen.open');

  @override
  Future<void> ring(String caller, String callId, {required bool notify, bool video = false}) =>
      _channel.invokeMethod<void>('ring.start', {'caller': caller, 'id': callId, 'notify': notify, 'video': video});

  @override
  Future<void> stop(String callId) => _channel.invokeMethod<void>('ring.stop', {'id': callId});

  /// The phone's name as its owner set it, else its model.
  Future<String> deviceName() async => await _channel.invokeMethod<String>('device.name') ?? 'Android';
}

class AndroidCallWindow extends ChangeNotifier implements CallWindow {
  AndroidCallWindow._();

  @override
  bool shown = false;

  void _set(bool on) {
    if (on == shown) return;
    shown = on;
    notifyListeners();
  }

  @override
  void finish() {
    _set(false);
    AndroidPlatform._channel.invokeMethod<void>('callwindow.finish');
  }
}
