/// The app's child processes, kept clear of an open camera. libwebrtc opens
/// the camera's device without close-on-exec, so a child started while it
/// is open keeps it open, and the next call's camera opens with no error
/// and gives no picture until that child exits (flutter_webrtc issue 2225,
/// ticket 02 of .scratch/tailnet-video). So while a camera is on no child
/// starts, and a camera opens only once those already started have exited.
library;

import 'dart:async';

class ChildProcesses {
  bool _cameraOn = false;
  int _running = 0;
  Completer<void>? _none;

  bool get cameraOn => _cameraOn;

  /// Run a short-lived child, `run`, unless a camera is on: then null, and
  /// nothing started.
  Future<T?> run<T>(Future<T> Function() run) async {
    if (_cameraOn) return null;
    _running++;
    try {
      return await run();
    } finally {
      if (--_running == 0) {
        _none?.complete();
        _none = null;
      }
    }
  }

  /// Whether a long-lived child may start now: not while a camera is on.
  bool get mayStart => !_cameraOn;

  /// A camera is opening: no child starts from now on, and this waits for
  /// those still running, `wait` at most. One that runs longer was started
  /// before the camera opened, and has no hold on it.
  Future<void> cameraOpening({Duration wait = const Duration(seconds: 2)}) async {
    _cameraOn = true;
    if (_running == 0) return;
    await (_none ??= Completer<void>()).future.timeout(wait, onTimeout: () {});
  }

  void cameraClosed() => _cameraOn = false;
}
