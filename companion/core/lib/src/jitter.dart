/// A call's jitter buffer (protocol section 4.8): packets arrive when the
/// network lets them, and leave one every 20 ms, in sequence, after 60 to
/// 200 ms in the buffer. A packet missing at its turn is concealed by the
/// decoder; one arriving after its turn is dropped. Pure: the caller's
/// clock pulls.
library;

import 'dart:collection';
import 'dart:typed_data';

/// What to do for the next 20 ms.
sealed class JitterPull {
  const JitterPull();
}

/// Decode this packet.
final class Play extends JitterPull {
  const Play(this.packet);
  final Uint8List packet;
}

/// The packet is missing: let the decoder conceal it.
final class Conceal extends JitterPull {
  const Conceal();
}

/// Still filling: play nothing (silence).
final class Wait extends JitterPull {
  const Wait();
}

class JitterBuffer {
  JitterBuffer({int targetMs = 60, int maxMs = 200, this.frameMs = 20})
    : _target = targetMs ~/ frameMs,
      _max = maxMs ~/ frameMs;

  final int frameMs;
  int _target;
  final int _max;
  final SplayTreeMap<int, Uint8List> _queue = SplayTreeMap();
  bool _playing = false;
  bool _ranDry = false;
  int? _next;

  /// Packets that came after their turn.
  int late = 0;

  /// Turns with no packet, concealed.
  int concealed = 0;

  /// How long the buffer holds audio before playing it.
  int get delayMs => _target * frameMs;

  /// The audio queued now.
  int get queuedMs => _queue.length * frameMs;

  /// A sequence number this far from the expected one is a sender that
  /// started again, not a late or early packet.
  static const _restart = 50;

  void push(int seq, Uint8List packet) {
    final next = _next;
    if (next != null && (seq - next).abs() > _restart) {
      _queue.clear();
      _playing = false;
      _next = null;
    } else if (next != null && seq < next) {
      late++;
      return;
    }
    _queue[seq] = packet;
    if (_queue.length > _max) {
      while (_queue.length > _target) {
        _queue.remove(_queue.firstKey());
      }
      if (_playing) _next = _queue.firstKey();
    }
  }

  JitterPull pull() {
    if (!_playing) {
      if (_queue.length < _target) return const Wait();
      _playing = true;
      _ranDry = false;
      _next = _queue.firstKey();
    }
    final packet = _queue.remove(_next);
    if (packet != null) {
      _next = _next! + 1;
      return Play(packet);
    }
    concealed++;
    if (_queue.isNotEmpty) {
      _next = _next! + 1; // lost: conceal it and go on
      return const Conceal();
    }
    // Ran dry: conceal once, then fill again, holding a frame more.
    if (!_ranDry) {
      _ranDry = true;
      if (_target < _max) _target++;
    }
    _playing = false;
    _next = _next! + 1;
    return const Conceal();
  }
}
