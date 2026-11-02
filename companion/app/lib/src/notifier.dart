/// Desktop notifications for messages, requests and calls while the app is
/// not in front (user story 55). flutter_local_notifications speaks to the
/// freedesktop notification service over D-Bus on Linux, and to Android's
/// notification manager on Android, on the channel `messages`;
/// there notifications are a runtime permission (permissions.dart).
library;

import 'package:flutter_local_notifications/flutter_local_notifications.dart';

abstract interface class Notifier {
  /// Show a notification; `conversation` is the contact's key it opens.
  Future<void> show(String title, String body, {String? conversation});

  /// A call coming in, with Answer and Decline, until [stopRinging].
  Future<void> ringing(String caller, String callId);
  Future<void> stopRinging(String callId);
}

class SystemNotifier implements Notifier {
  SystemNotifier({this.onOpen, this.onCall});

  /// Called with a notification's conversation when the user clicks it.
  final void Function(String? conversation)? onOpen;

  /// A ringing notification's button: answer (true) or decline (false);
  /// a click elsewhere on it is null, which shows the call.
  final void Function(String callId, bool? answer)? onCall;
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
  Future<void> ringing(String caller, String callId) async {
    if (!_ready) return;
    final id = _ringing[callId] = _next++ & 0x7fffffff;
    await _plugin.show(
      id: id,
      title: 'Call from $caller',
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
        android: AndroidNotificationDetails(
          'calls',
          'Calls',
          importance: Importance.max,
          category: AndroidNotificationCategory.call,
          fullScreenIntent: true,
          actions: [
            AndroidNotificationAction('answer', 'Answer', showsUserInterface: true),
            AndroidNotificationAction('decline', 'Decline'),
          ],
        ),
      ),
    );
  }

  @override
  Future<void> stopRinging(String callId) async {
    final id = _ringing.remove(callId);
    if (id != null && _ready) await _plugin.cancel(id: id);
  }
}
