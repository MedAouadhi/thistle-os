/// A WebRTC call's media, beside the audio devices a `ws-opus` call uses:
/// the app's one seam to WebRTC (tailnet-video spec, "The companion app").
/// Talk drives it; flutter_webrtc implements it (webrtc_engine.dart), and a
/// fake stands in for tests. The SDP and candidates it gives and takes are
/// opaque to the app: the core carries them as `call_rtc` frames, the
/// tailnet's candidates alone. A camera is switched by replacing the video
/// track, never by renegotiating.
library;

import 'package:flutter/widgets.dart' show Widget;
import 'package:tailnet_chat_core/tailnet_chat_core.dart' show MediaState, RtcPayload;

/// Our SDP or one of our candidates, for the other side.
typedef SendRtc = void Function(RtcPayload payload);

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

/// A camera that opened but gave no picture: held by another process (on
/// Linux, flutter_webrtc issue 2225), or refused.
class CameraUnavailable implements Exception {
  const CameraUnavailable(this.why);
  final String why;
  @override
  String toString() => why;
}

/// Capture and sending are capped at 1280x720, 30 fps (tailnet-video spec,
/// Quality); WebRTC's congestion control does the rest.
const maxVideoWidth = 1280, maxVideoHeight = 720, maxVideoFps = 30;

/// What a call's video has done so far, for the log.
class VideoStats {
  const VideoStats({
    this.framesSent = 0,
    this.framesReceived = 0,
    this.sent,
    this.received,
    this.fpsSent,
    this.limitedBy,
  });
  final int framesSent, framesReceived;

  /// The size of the last frame sent and received, `1280x720`.
  final String? sent, received;
  final num? fpsSent;

  /// What holds the sending back, as WebRTC says: `bandwidth`, `cpu`.
  final String? limitedBy;

  @override
  String toString() =>
      'sent $framesSent frames${sent == null ? '' : ' at $sent'}${fpsSent == null ? '' : ', $fpsSent fps'}'
      '${limitedBy == null || limitedBy == 'none' ? '' : ', limited by $limitedBy'}, '
      'received $framesReceived${received == null ? '' : ' at $received'}';
}

abstract interface class CallEngine {
  /// The cameras this machine has, the usual one first: what
  /// [CallSession.setCamera] takes, in the order Switch camera goes round.
  /// Empty without one.
  Future<List<String>> cameras();

  /// Open `camera`, one of [cameras], before a call's media starts: the
  /// caller's own picture while it rings. Throws [CameraUnavailable] if it
  /// gives no picture.
  Future<CameraPreview> preview(String camera);

  /// Start a call's media: the microphone, the camera off unless `camera`
  /// is given, one audio and one video track negotiated both ways, no ICE
  /// server. The caller (`offerer`) makes the SDP offer. `send` takes what
  /// is to go to the other side, `onState` the connection's state as it
  /// changes. A `camera` opened by [preview] becomes the session's, its
  /// picture sent once the media connects; closing the session closes it.
  Future<CallSession> start({
    required bool offerer,
    required SendRtc send,
    required void Function(MediaState state) onState,
    CameraPreview? camera,
  });
}

/// A camera opened while a call rings, before its media: our own picture.
abstract interface class CameraPreview {
  /// Which of [CallEngine.cameras] it is.
  String get camera;
  Widget view();

  /// Let the camera go; once given to [CallEngine.start], the session does.
  Future<void> close();
}

abstract interface class CallSession {
  /// What removes the speaker from the microphone, for the call screen.
  String get echoCancellation;

  /// Take the other side's SDP or candidate, in the order they came.
  Future<void> take(RtcPayload payload);

  set muted(bool on);

  /// 0 to 2; 1 plays the other side as it comes.
  set volume(double v);

  /// The candidate pair in use, once connected; null if it cannot be told.
  Future<CallPath?> selectedPath();

  /// Send `camera`'s picture, one of [CallEngine.cameras], or none with
  /// null: the video track is replaced. Throws [CameraUnavailable] if the
  /// camera gives no picture, which leaves it off.
  Future<void> setCamera(String? camera);

  /// The video's frames so far; null if it cannot be told.
  Future<VideoStats?> videoStats();

  /// The other side's video, and our own camera's, to draw.
  Widget remoteVideo();
  Widget localVideo();

  /// Hang up the media: the microphone, the camera and the connection.
  Future<void> close();
}
