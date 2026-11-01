/// Opus at 16 kHz mono, 20 ms frames, through the system's libopus by FFI
/// (`libopus.so.0` on Linux, Debian/Ubuntu package `libopus0`; on Android
/// the app bundles its own, ticket 11). Voice messages and calls both use
/// it: [VoiceRecorder] and [decodeVoice] for messages, the encoder and
/// decoder for a call's packets.
library;

import 'dart:ffi';
import 'dart:io';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';

import 'ogg_opus.dart';
import 'protocol.dart' show maxVoiceMs;

const sampleRate = 16000;
const frameMs = 20;
const samplesPerFrame = sampleRate * frameMs ~/ 1000; // 320

/// libopus could not be loaded.
class OpusUnavailable implements Exception {
  OpusUnavailable(this.why);
  final String why;
  @override
  String toString() => 'Opus is unavailable ($why); on Debian or Ubuntu: sudo apt install libopus0';
}

typedef _EncCreateC = Pointer<Void> Function(Int32, Int32, Int32, Pointer<Int32>);
typedef _EncCreate = Pointer<Void> Function(int, int, int, Pointer<Int32>);
typedef _EncodeC = Int32 Function(Pointer<Void>, Pointer<Int16>, Int32, Pointer<Uint8>, Int32);
typedef _Encode = int Function(Pointer<Void>, Pointer<Int16>, int, Pointer<Uint8>, int);
typedef _CtlIntC = Int32 Function(Pointer<Void>, Int32, VarArgs<(Int32,)>);
typedef _CtlInt = int Function(Pointer<Void>, int, int);
typedef _CtlPtrC = Int32 Function(Pointer<Void>, Int32, VarArgs<(Pointer<Int32>,)>);
typedef _CtlPtr = int Function(Pointer<Void>, int, Pointer<Int32>);
typedef _DecCreateC = Pointer<Void> Function(Int32, Int32, Pointer<Int32>);
typedef _DecCreate = Pointer<Void> Function(int, int, Pointer<Int32>);
typedef _DecodeC = Int32 Function(Pointer<Void>, Pointer<Uint8>, Int32, Pointer<Int16>, Int32, Int32);
typedef _Decode = int Function(Pointer<Void>, Pointer<Uint8>, int, Pointer<Int16>, int, int);
typedef _DestroyC = Void Function(Pointer<Void>);
typedef _Destroy = void Function(Pointer<Void>);

class _Lib {
  _Lib(DynamicLibrary l)
    : encCreate = l.lookupFunction<_EncCreateC, _EncCreate>('opus_encoder_create'),
      encode = l.lookupFunction<_EncodeC, _Encode>('opus_encode'),
      encCtlInt = l.lookupFunction<_CtlIntC, _CtlInt>('opus_encoder_ctl'),
      encCtlPtr = l.lookupFunction<_CtlPtrC, _CtlPtr>('opus_encoder_ctl'),
      encDestroy = l.lookupFunction<_DestroyC, _Destroy>('opus_encoder_destroy'),
      decCreate = l.lookupFunction<_DecCreateC, _DecCreate>('opus_decoder_create'),
      decode = l.lookupFunction<_DecodeC, _Decode>('opus_decode'),
      decDestroy = l.lookupFunction<_DestroyC, _Destroy>('opus_decoder_destroy');

  final _EncCreate encCreate;
  final _Encode encode;
  final _CtlInt encCtlInt;
  final _CtlPtr encCtlPtr;
  final _Destroy encDestroy;
  final _DecCreate decCreate;
  final _Decode decode;
  final _Destroy decDestroy;

  static _Lib? _it;
  static _Lib get it => _it ??= _Lib(_open());

  static DynamicLibrary _open() {
    final names = Platform.isAndroid
        ? ['libopus.so']
        : Platform.isMacOS
        ? ['libopus.0.dylib', 'libopus.dylib']
        : ['libopus.so.0', 'libopus.so'];
    Object? last;
    for (final n in names) {
      try {
        return DynamicLibrary.open(n);
      } on Object catch (e) {
        last = e;
      }
    }
    throw OpusUnavailable('$last');
  }
}

/// Whether libopus can be loaded here.
bool get opusAvailable {
  try {
    _Lib.it;
    return true;
  } on OpusUnavailable {
    return false;
  }
}

const _applicationVoip = 2048;
const _setBitrate = 4002;
const _setInbandFec = 4012;
const _setPacketLossPerc = 4014;
const _getLookahead = 4027;

/// 20 ms of 16 kHz mono PCM in, one Opus packet out.
class OpusEncoder {
  OpusEncoder({int bitrate = 24000}) {
    final err = calloc<Int32>();
    try {
      _enc = _lib.encCreate(sampleRate, 1, _applicationVoip, err);
      if (err.value != 0 || _enc == nullptr) throw OpusUnavailable('opus_encoder_create: ${err.value}');
    } finally {
      calloc.free(err);
    }
    _lib.encCtlInt(_enc, _setBitrate, bitrate);
    _lib.encCtlInt(_enc, _setInbandFec, 1);
    _lib.encCtlInt(_enc, _setPacketLossPerc, 5);
  }

  final _Lib _lib = _Lib.it;
  late final Pointer<Void> _enc;
  final Pointer<Int16> _pcm = calloc<Int16>(samplesPerFrame);
  final Pointer<Uint8> _out = calloc<Uint8>(1500);
  bool _disposed = false;

  /// Samples at 48 kHz the decoder's output lags the input by: the Ogg
  /// header's pre-skip.
  int get preSkip {
    final p = calloc<Int32>();
    try {
      _lib.encCtlPtr(_enc, _getLookahead, p);
      return p.value * 48000 ~/ sampleRate;
    } finally {
      calloc.free(p);
    }
  }

  Uint8List encode(Int16List frame) {
    if (frame.length != samplesPerFrame) throw ArgumentError('a frame is $samplesPerFrame samples');
    _pcm.asTypedList(samplesPerFrame).setAll(0, frame);
    final n = _lib.encode(_enc, _pcm, samplesPerFrame, _out, 1500);
    if (n < 0) throw StateError('opus_encode: $n');
    return Uint8List.fromList(_out.asTypedList(n));
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _lib.encDestroy(_enc);
    calloc.free(_pcm);
    calloc.free(_out);
  }
}

/// One Opus packet in, 20 ms of 16 kHz mono PCM out; null conceals a lost one.
class OpusDecoder {
  OpusDecoder() {
    final err = calloc<Int32>();
    try {
      _dec = _lib.decCreate(sampleRate, 1, err);
      if (err.value != 0 || _dec == nullptr) throw OpusUnavailable('opus_decoder_create: ${err.value}');
    } finally {
      calloc.free(err);
    }
  }

  final _Lib _lib = _Lib.it;
  late final Pointer<Void> _dec;
  // Up to 120 ms a packet, as a sender may pack several frames.
  static const _maxSamples = sampleRate * 120 ~/ 1000;
  final Pointer<Int16> _pcm = calloc<Int16>(_maxSamples);
  final Pointer<Uint8> _in = calloc<Uint8>(4096);
  bool _disposed = false;

  Int16List decode(Uint8List? packet) {
    final int n;
    if (packet == null || packet.isEmpty) {
      n = _lib.decode(_dec, nullptr, 0, _pcm, samplesPerFrame, 0);
    } else {
      if (packet.length > 4096) throw ArgumentError('an Opus packet is at most 4096 bytes here');
      _in.asTypedList(packet.length).setAll(0, packet);
      n = _lib.decode(_dec, _in, packet.length, _pcm, _maxSamples, 0);
    }
    if (n < 0) return Int16List(samplesPerFrame); // a corrupt packet: silence
    return Int16List.fromList(_pcm.asTypedList(n));
  }

  void dispose() {
    if (_disposed) return;
    _disposed = true;
    _lib.decDestroy(_dec);
    calloc.free(_pcm);
    calloc.free(_in);
  }
}

/// A voice message, recorded.
class VoiceMessage {
  VoiceMessage(this.bytes, this.ms);
  final Uint8List bytes;
  final int ms;
}

/// Records a voice message: PCM in, in chunks of any size; an Ogg/Opus
/// file out, at most two minutes.
class VoiceRecorder {
  final OpusEncoder _enc = OpusEncoder();
  final List<Uint8List> _packets = [];
  final Int16List _frame = Int16List(samplesPerFrame);
  int _filled = 0;
  int _samples = 0;
  bool _done = false;

  static const _maxSamples = maxVoiceMs * sampleRate ~/ 1000;

  /// The length recorded so far.
  int get ms => _samples * 1000 ~/ sampleRate;

  /// Two minutes recorded: more is ignored.
  bool get full => _samples >= _maxSamples;

  void add(List<int> pcm) {
    if (_done) throw StateError('finished');
    for (final s in pcm) {
      if (full) return;
      _frame[_filled++] = s;
      _samples++;
      if (_filled == samplesPerFrame) {
        _packets.add(_enc.encode(_frame));
        _filled = 0;
      }
    }
  }

  /// The file: the last frame padded, and the encoder's lookahead flushed
  /// with one more frame of silence so the end is not cut.
  VoiceMessage finish() {
    if (_done) throw StateError('finished');
    _done = true;
    try {
      if (_filled > 0) {
        _frame.fillRange(_filled, samplesPerFrame, 0);
        _packets.add(_enc.encode(_frame));
      }
      _packets.add(_enc.encode(Int16List(samplesPerFrame)));
      return VoiceMessage(writeOggOpus(_packets, ms: ms, preSkip: _enc.preSkip), ms);
    } finally {
      _enc.dispose();
    }
  }

  /// Throw it away.
  void cancel() {
    if (_done) return;
    _done = true;
    _enc.dispose();
  }
}

/// A voice message's audio, 16 kHz mono, its pre-skip dropped and its end
/// trimmed to its length.
Int16List decodeVoice(List<int> oggOpus) {
  final file = readOggOpus(oggOpus);
  if (file.channels != 1) throw const FormatException('a voice message is mono');
  final dec = OpusDecoder();
  try {
    final out = BytesBuilder(copy: false);
    for (final p in file.packets) {
      out.add(dec.decode(p).buffer.asUint8List());
    }
    final all = out.takeBytes().buffer.asInt16List();
    final skip = file.preSkip * sampleRate ~/ 48000;
    final want = file.durationMs * sampleRate ~/ 1000;
    final end = (skip + want).clamp(skip, all.length);
    return Int16List.fromList(all.sublist(skip.clamp(0, all.length), end));
  } finally {
    dec.dispose();
  }
}
