/// A connected call's audio: the microphone encoded into `ws-opus` frames,
/// and the other side's frames through a jitter buffer and the decoder to
/// the speaker. The devices are the platform's ([AudioOut], a PCM stream);
/// the frames are [ChatPeer]'s.
library;

import 'dart:async';
import 'dart:typed_data';

import 'jitter.dart';
import 'opus.dart';
import 'peer.dart';

/// Where decoded audio goes: 16 kHz mono PCM, 20 ms at a time.
abstract interface class AudioOut {
  void write(Int16List pcm);
  Future<void> close();
}

/// Audio that goes nowhere.
class NoAudioOut implements AudioOut {
  @override
  void write(Int16List pcm) {}
  @override
  Future<void> close() async {}
}

/// 20 ms frames from `frame(i)`, paced by the clock (a late timer catches
/// up): a microphone stand-in for tests and the headless core.
Stream<Int16List> pacedAudio(Int16List Function(int index) frame) {
  late StreamController<Int16List> c;
  Timer? timer;
  var sent = 0;
  final clock = Stopwatch();
  c = StreamController<Int16List>(
    onListen: () {
      clock.start();
      timer = Timer.periodic(const Duration(milliseconds: frameMs), (_) {
        final due = clock.elapsedMilliseconds ~/ frameMs;
        while (sent < due && !c.isClosed) {
          c.add(frame(sent++));
        }
      });
    },
    onCancel: () {
      timer?.cancel();
      return c.close();
    },
  );
  return c.stream;
}

class CallMedia {
  CallMedia(this.peer, this.call, {required Stream<List<int>> mic, required this.speaker}) : _micStream = mic;

  final ChatPeer peer;
  final String call;
  final AudioOut speaker;
  final Stream<List<int>> _micStream;

  /// Send silence instead of the microphone; frames keep going, so the
  /// other side does not time out.
  bool muted = false;

  /// What the speaker gets: 0 to 2, 1 as received.
  double volume = 1;

  final JitterBuffer jitter = JitterBuffer();
  OpusEncoder? _enc;
  OpusDecoder? _dec;
  StreamSubscription<List<int>>? _mic;
  StreamSubscription<CallAudio>? _in;
  StreamSubscription<ChatEvent>? _events;
  Timer? _player;
  final Stopwatch _clock = Stopwatch();
  int _pulled = 0;
  final Int16List _frame = Int16List(samplesPerFrame);
  int _filled = 0;
  int _seq = 0;
  bool _started = false;
  bool stopped = false;

  /// Frames sent and played, for a log line or a test.
  int get sent => _seq;
  int get played => _pulled;

  void start() {
    if (_started) return;
    _started = true;
    _enc = OpusEncoder();
    _dec = OpusDecoder();
    _in = peer.callAudio.where((a) => a.call == call).listen((a) => jitter.push(a.seq, a.packet));
    _events = peer.events.where((e) => e.kind == 'call' && e.id == call).listen((_) {
      if (!(peer.calls[call]?.live ?? false)) unawaited(stop());
    });
    _mic = _micStream.listen(_onMic);
    _clock.start();
    _player = Timer.periodic(const Duration(milliseconds: frameMs), (_) => _play());
  }

  void _onMic(List<int> pcm) {
    for (final s in pcm) {
      _frame[_filled++] = s;
      if (_filled == samplesPerFrame) {
        _filled = 0;
        if (muted) _frame.fillRange(0, samplesPerFrame, 0);
        peer.sendCallAudio(call, _seq++, _enc!.encode(_frame));
      }
    }
  }

  void _play() {
    final due = _clock.elapsedMilliseconds ~/ frameMs;
    while (_pulled < due && !stopped) {
      _pulled++;
      final Int16List? pcm = switch (jitter.pull()) {
        Play(:final packet) => _dec!.decode(packet),
        Conceal() => _dec!.decode(null),
        Wait() => null,
      };
      if (pcm == null) continue;
      if (volume != 1) {
        for (var i = 0; i < pcm.length; i++) {
          pcm[i] = (pcm[i] * volume).round().clamp(-32768, 32767);
        }
      }
      speaker.write(pcm);
    }
  }

  Future<void> stop() async {
    if (stopped) return;
    stopped = true;
    _player?.cancel();
    await _mic?.cancel();
    await _in?.cancel();
    await _events?.cancel();
    _enc?.dispose();
    _dec?.dispose();
    await speaker.close();
  }
}
