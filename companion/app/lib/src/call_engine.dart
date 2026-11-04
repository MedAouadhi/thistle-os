/// A WebRTC call's media, beside the audio devices a `ws-opus` call uses:
/// the app's one seam to WebRTC (tailnet-video spec, "The companion app").
/// Talk drives it; flutter_webrtc implements it (webrtc_engine.dart), and a
/// fake stands in for tests. The SDP and candidates it gives and takes are
/// opaque to the app: the core carries them as `call_rtc` frames, the
/// tailnet's candidates alone.
library;

/// Our SDP or one of our candidates, for the other side.
typedef SendRtc = void Function({Map<String, Object?>? sdp, Map<String, Object?>? candidate});

/// The two ends of the candidate pair the connection selected.
class CallPath {
  const CallPath({required this.local, required this.remote, this.localType = '?', this.remoteType = '?'});
  final String local;
  final String remote;
  final String localType;
  final String remoteType;
  @override
  String toString() => '$localType $local to $remoteType $remote';
}

abstract interface class CallEngine {
  /// Start a call's media: the microphone, the camera off, one audio and one
  /// video track negotiated both ways, no ICE server. The caller (`offerer`)
  /// makes the SDP offer. `send` takes what is to go to the other side,
  /// `onState` the connection's state as it changes: `connected`,
  /// `disconnected`, `failed`.
  Future<CallSession> start({
    required bool offerer,
    required SendRtc send,
    required void Function(String state) onState,
  });
}

abstract interface class CallSession {
  /// What removes the speaker from the microphone, for the call screen.
  String get echoCancellation;

  /// Take the other side's SDP or candidate, in the order they came.
  Future<void> take({Map<String, Object?>? sdp, Map<String, Object?>? candidate});

  set muted(bool on);

  /// 0 to 2; 1 plays the other side as it comes.
  set volume(double v);

  /// The candidate pair in use, once connected; null if it cannot be told.
  Future<CallPath?> selectedPath();

  /// Hang up the media: the microphone and the connection.
  Future<void> close();
}
