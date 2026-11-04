/// Desktop notifications for messages, requests and calls while the app is
/// not in front (user story 55). flutter_local_notifications speaks to the
/// freedesktop notification service over D-Bus on Linux, and to Android's
/// notification manager on Android, on the channel `messages`;
/// there notifications are a runtime permission (permissions.dart).
///
/// A call on Android rings like a phone call instead (tailnet-video spec, A
/// call to a locked phone): the [CallRinger] the app's channel gives
/// (android.dart, CallRing.kt) plays the phone's ringtone and shows the
/// call-style notification and the call window over the lock screen.
library;

import 'package:flutter/foundation.dart' show Listenable;
import 'package:flutter_local_notifications/flutter_local_notifications.dart';

abstract interface class Notifier {
  /// Show a notification; `conversation` is the contact's key it opens.
  Future<void> show(String title, String body, {String? conversation});

  /// Whether [ringing] rings itself (Android: the phone's ringtone); if
  /// not, the app plays its own ring.
  bool get ringsItself;

  /// A call coming in, until [stopRinging]: with the app out of sight
  /// ([inFront] false), a notification with Answer and Decline, saying
  /// Video call when the caller's camera is on ([video]).
  Future<void> ringing(String caller, String callId, {bool inFront = false, bool video = false});
  Future<void> stopRinging(String callId);
}

/// The phone's own ring for a call: its ringtone, and unless [notify] is
/// false the call-style notification, which opens the call window on a
/// locked phone. Android's (android.dart).
abstract interface class CallRinger {
  Future<void> ring(String caller, String callId, {required bool notify, bool video = false});
  Future<void> stop(String callId);
}

/// Android's call window over the lock screen (CallActivity.kt), on the
/// app's one engine: while it shows, the app draws the call screen and
/// nothing of the chats, and closing the call screen finishes the window,
/// so a locked phone is back at its lock screen.
abstract interface class CallWindow implements Listenable {
  bool get shown;
  void finish();
}

class SystemNotifier implements Notifier {
  SystemNotifier({this.onOpen, this.onCall, this.ringer});

  /// Called with a notification's conversation when the user clicks it.
  final void Function(String? conversation)? onOpen;

  /// A ringing notification's button: answer (true) or decline (false);
  /// a click elsewhere on it is null, which shows the call.
  final void Function(String callId, bool? answer)? onCall;

  /// Android's ring; Linux has none, and rings with a notification while
  /// the app plays its two-tone ring.
  final CallRinger? ringer;
  final Map<String, int> _ringing = {};
  final _plugin = FlutterLocalNotificationsPlugin();
  bool _ready = false;
  int _next = 0;

  Future<void> init() async {
    try {
      _ready =
          await _plugin.initialize(
            settings: const InitializationSettings(
              linux: LinuxInitializationSettings(defaultActionName: 'Open'),
              android: AndroidInitializationSettings('ic_stat_chat'),
            ),
            onDidReceiveNotificationResponse: (r) {
              final payload = r.payload;
              if (payload != null && payload.startsWith('call:')) {
                final id = payload.substring(5);
                onCall?.call(id, switch (r.actionId) {
                  'answer' => true,
                  'decline' => false,
                  _ => null,
                });
              } else {
                onOpen?.call(payload);
              }
            },
          ) ??
          false;
    } on Object {
      _ready = false; // no notification service: the app works without
    }
  }

  @override
  Future<void> show(String title, String body, {String? conversation}) async {
    if (!_ready) return;
    await _plugin.show(
      id: _next++ & 0x7fffffff,
      title: title,
      body: body,
      payload: conversation,
      notificationDetails: const NotificationDetails(
        linux: LinuxNotificationDetails(category: LinuxNotificationCategory.imReceived),
        android: AndroidNotificationDetails('messages', 'Messages', importance: Importance.high),
      ),
    );
  }

  @override
  bool get ringsItself => ringer != null;

  @override
  Future<void> ringing(String caller, String callId, {bool inFront = false, bool video = false}) async {
    final phone = ringer;
    if (phone != null) return phone.ring(caller, callId, notify: !inFront, video: video);
    if (!_ready || inFront) return;
    final id = _ringing[callId] = _next++ & 0x7fffffff;
    await _plugin.show(
      id: id,
      title: '${video ? 'Video call' : 'Call'} from $caller',
      body: 'Tailnet call',
      payload: 'call:$callId',
      notificationDetails: const NotificationDetails(
        linux: LinuxNotificationDetails(
          category: LinuxNotificationCategory.im,
          urgency: LinuxNotificationUrgency.critical,
          resident: true,
          actions: [
            LinuxNotificationAction(key: 'answer', label: 'Answer'),
            LinuxNotificationAction(key: 'decline', label: 'Decline'),
          ],
        ),
      ),
    );
  }

  @override
  Future<void> stopRinging(String callId) async {
    await ringer?.stop(callId);
    final id = _ringing.remove(callId);
    if (id != null && _ready) await _plugin.cancel(id: id);
  }
}
