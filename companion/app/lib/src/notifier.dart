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
}

class SystemNotifier implements Notifier {
  SystemNotifier({this.onOpen});

  /// Called with a notification's conversation when the user clicks it.
  final void Function(String? conversation)? onOpen;
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
            onDidReceiveNotificationResponse: (r) => onOpen?.call(r.payload),
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
}
