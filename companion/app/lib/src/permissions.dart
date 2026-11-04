/// Android's runtime permissions, asked when first needed: notifications
/// when the background service starts or a contact is added, the
/// microphone when a voice message or a call first needs it (ticket 10),
/// the camera when it is first switched on in a call (tailnet-video).
/// A refusal is explained where the user sees it, with what it stops.
/// Linux asks for nothing: the model is given no [Permissions] there.
///
/// Calls filling the screen over the lock screen is not prompted for:
/// Android 14 and later grant it only to calling apps, and the user allows
/// it on a page of Android's own, which Settings points to when it is
/// missing (tailnet-video spec, The full-screen permission).
library;

import 'package:flutter/foundation.dart';

enum AppPermission { notifications, microphone, camera }

enum PermissionAnswer {
  granted,
  denied,

  /// Android will not show the prompt again; only its settings can allow it.
  permanentlyDenied,
}

/// The platform's side: its state and its prompt.
abstract interface class PermissionPlatform {
  Future<PermissionAnswer> status(AppPermission p);

  /// Show the system prompt (Android shows it only while it may).
  Future<PermissionAnswer> request(AppPermission p);
  Future<void> openSettings();

  /// Whether a call's notification may open its window over the lock screen.
  Future<bool> canFillScreen();

  /// Android's page that allows it.
  Future<void> openFillScreenSettings();
}

class Permissions extends ChangeNotifier {
  Permissions(this._platform);

  final PermissionPlatform _platform;
  final Map<AppPermission, PermissionAnswer> _refused = {};
  final Set<AppPermission> _dismissed = {};

  /// Ask for `p` unless it is granted; whether it is granted now. A
  /// permission refused for good is not asked again.
  Future<bool> need(AppPermission p) async {
    var answer = await _platform.status(p);
    if (answer == PermissionAnswer.denied) answer = await _platform.request(p);
    if (answer == PermissionAnswer.granted) {
      _refused.remove(p);
    } else {
      _refused[p] = answer;
      _dismissed.remove(p);
    }
    notifyListeners();
    return answer == PermissionAnswer.granted;
  }

  /// The refusals still to explain.
  Map<AppPermission, PermissionAnswer> get refusals => {
    for (final e in _refused.entries)
      if (!_dismissed.contains(e.key)) e.key: e.value,
  };

  bool forGood(AppPermission p) => _refused[p] == PermissionAnswer.permanentlyDenied;

  /// What a refusal of `p` stops, and how to undo it; null if not refused.
  String? refusal(AppPermission p) {
    final answer = refusals[p];
    if (answer == null) return null;
    final how = answer == PermissionAnswer.permanentlyDenied
        ? 'Allow ${_it(p)} in Android\'s settings for Thistle Chat.'
        : 'Allow ${_it(p)} when asked again, or in Android\'s settings for Thistle Chat.';
    return switch (p) {
      AppPermission.notifications =>
        'Notifications are not allowed, so messages and calls that come while the app is in the background '
            'go unnoticed, and the notification that keeps it listening is hidden. $how',
      AppPermission.microphone => 'The microphone is not allowed, so voice messages and calls cannot be recorded. $how',
      AppPermission.camera => 'The camera is not allowed, so the other side of a call sees No camera. $how',
    };
  }

  static String _it(AppPermission p) => p == AppPermission.notifications ? 'them' : 'it';

  void dismiss(AppPermission p) {
    _dismissed.add(p);
    notifyListeners();
  }

  Future<void> openSettings() => _platform.openSettings();

  /// Whether calls fill the screen; assumed until checked. Without it a
  /// call rings as its notification alone.
  bool get callsFillScreen => _fillScreen;
  bool _fillScreen = true;

  /// Check again; at start, when a call comes, and back from the settings.
  Future<void> checkFillScreen() async {
    final now = await _platform.canFillScreen();
    if (now == _fillScreen) return;
    _fillScreen = now;
    notifyListeners();
  }

  Future<void> openFillScreenSettings() => _platform.openFillScreenSettings();
}
