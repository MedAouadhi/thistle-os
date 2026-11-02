/// The microphone and the speaker on Android, over the app's own audio
/// channels (`android/app/src/main/kotlin/.../Audio.kt`): AudioRecord from
/// the voice-communication source with the platform's AcousticEchoCanceler
/// for a call, and AudioTrack. 16 kHz mono 16-bit, 20 ms a frame, the
/// microphone's frames on an event channel and the speaker's on the method
/// channel. A call also puts the phone in communication mode and the
/// listening service in the foreground as using the microphone, which
/// Android 14 requires to record with the app in the background.
library;

import 'dart:async';
import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart' show AudioOut;

import 'devices.dart';

class AndroidAudioDevices implements AudioDevices {
  static const control = MethodChannel('org.thistleos.thistle_chat/audio');
  static const microphone = EventChannel('org.thistleos.thistle_chat/microphone');

  @override
  Future<Stream<Int16List>> recorder() async => (await _microphone(call: false)).$1;

  @override
  Future<AudioOut> player({bool ring = false}) => _speaker(ring ? 'ring' : 'voice');

  @override
  Future<CallDevices> call() async {
    // Communication mode, the proximity sensor, and the listening service
    // as using the microphone; undone by call.end.
    await _invoke<void>('call.begin');
    try {
      final (mic, echo) = await _microphone(call: true);
      final AudioOut speaker;
      try {
        speaker = await _speaker('call');
      } on AudioUnavailable {
        await _invoke<void>('mic.close'); // opened, never listened to
        rethrow;
      }
      return CallDevices(
        mic,
        speaker,
        echo ?? 'none (this phone has no AcousticEchoCanceler)',
        () async => _invoke<void>('call.end'),
      );
    } on AudioUnavailable {
      await _invoke<void>('call.end');
      rethrow;
    }
  }

  /// The microphone's frames, and what cancels its echo.
  Future<(Stream<Int16List>, String?)> _microphone({required bool call}) async {
    final echo = await _invoke<String>('mic.open', {'call': call});
    StreamSubscription<dynamic>? frames;
    late final StreamController<Int16List> out;
    out = StreamController<Int16List>(
      onListen: () {
        frames = microphone.receiveBroadcastStream().listen((bytes) {
          var b = bytes as Uint8List;
          if (b.offsetInBytes.isOdd) b = Uint8List.fromList(b); // a view needs aligned samples
          out.add(b.buffer.asInt16List(b.offsetInBytes, b.lengthInBytes ~/ 2));
        }, onError: (Object e) => out.addError(_unavailable(e)));
      },
      onCancel: () async {
        await frames?.cancel();
        await _invoke<void>('mic.close');
        await out.close();
      },
    );
    return (out.stream, echo);
  }

  /// A speaker: `voice` (media), `ring` (ringtone) or `call` (the call's
  /// voice stream, through the echo canceller).
  Future<AudioOut> _speaker(String use) async {
    final id = await _invoke<int>('speaker.open', {'use': use});
    return _AndroidSpeaker(id!);
  }

  static Future<T?> _invoke<T>(String method, [Object? args]) async {
    try {
      return await control.invokeMethod<T>(method, args);
    } on PlatformException catch (e) {
      throw _unavailable(e);
    }
  }

  static AudioUnavailable _unavailable(Object e) =>
      AudioUnavailable(e is PlatformException ? (e.message ?? e.code) : '$e');
}

class _AndroidSpeaker implements AudioOut {
  _AndroidSpeaker(this._id);
  final int _id;
  bool _closed = false;

  @override
  void write(Int16List pcm) {
    if (_closed || pcm.isEmpty) return;
    final bytes = Uint8List.fromList(pcm.buffer.asUint8List(pcm.offsetInBytes, pcm.lengthInBytes));
    // Fire and forget, in order; a speaker that went away loses the frame.
    unawaited(
      AndroidAudioDevices.control
          .invokeMethod<void>('speaker.write', {'id': _id, 'pcm': bytes})
          .catchError((Object _) {}),
    );
  }

  /// Plays out what was written, then lets the speaker go.
  @override
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    try {
      await AndroidAudioDevices.control.invokeMethod<void>('speaker.close', {'id': _id});
    } on PlatformException {
      // already gone
    }
  }
}
