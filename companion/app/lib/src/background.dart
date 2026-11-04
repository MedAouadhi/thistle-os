/// Keeping the app reachable in the background (spec, The companion app:
/// Listening). On Android a foreground service with a persistent
/// notification keeps the process, and the Dart isolate that listens,
/// alive after the app is swiped away; on by default, switched in
/// Settings. Linux listens while the app runs and has no service.
library;

abstract interface class BackgroundService {
  /// Start or stop the service; idempotent.
  Future<void> setRunning(bool on);
}
