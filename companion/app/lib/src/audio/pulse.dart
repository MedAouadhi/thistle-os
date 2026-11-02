/// The microphone and the speaker on Linux: PulseAudio's simple API
/// (`libpulse-simple.so.0`) by FFI, which PipeWire answers through
/// pipewire-pulse as well. Each stream runs in an isolate of its own,
/// since its reads and writes block; 16 kHz mono 16-bit, 20 ms at a time.
library;

import 'dart:async';
import 'dart:ffi';
import 'dart:isolate';
import 'dart:typed_data';

import 'package:ffi/ffi.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart' show AudioOut, sampleRate, samplesPerFrame;

/// No sound server, no such device, or no libpulse.
class AudioUnavailable implements Exception {
  AudioUnavailable(this.why);
  final String why;
  @override
  String toString() => 'no audio: $why';
}

final class _SampleSpec extends Struct {
  @Int32()
  external int format;
  @Uint32()
  external int rate;
  @Uint8()
  external int channels;
}

final class _BufferAttr extends Struct {
  @Uint32()
  external int maxlength;
  @Uint32()
  external int tlength;
  @Uint32()
  external int prebuf;
  @Uint32()
  external int minreq;
  @Uint32()
  external int fragsize;
}

typedef _NewC =
    Pointer<Void> Function(
      Pointer<Utf8>,
      Pointer<Utf8>,
      Int32,
      Pointer<Utf8>,
      Pointer<Utf8>,
      Pointer<_SampleSpec>,
      Pointer<Void>,
      Pointer<_BufferAttr>,
      Pointer<Int32>,
    );
typedef _New =
    Pointer<Void> Function(
      Pointer<Utf8>,
      Pointer<Utf8>,
      int,
      Pointer<Utf8>,
      Pointer<Utf8>,
      Pointer<_SampleSpec>,
      Pointer<Void>,
      Pointer<_BufferAttr>,
      Pointer<Int32>,
    );
typedef _IoC = Int32 Function(Pointer<Void>, Pointer<Void>, Size, Pointer<Int32>);
typedef _Io = int Function(Pointer<Void>, Pointer<Void>, int, Pointer<Int32>);
typedef _DrainC = Int32 Function(Pointer<Void>, Pointer<Int32>);
typedef _Drain = int Function(Pointer<Void>, Pointer<Int32>);
typedef _FreeC = Void Function(Pointer<Void>);
typedef _Free = void Function(Pointer<Void>);
typedef _StrerrorC = Pointer<Utf8> Function(Int32);
typedef _Strerror = Pointer<Utf8> Function(int);
typedef _SetenvC = Int32 Function(Pointer<Utf8>, Pointer<Utf8>, Int32);
typedef _Setenv = int Function(Pointer<Utf8>, Pointer<Utf8>, int);
typedef _UnsetenvC = Int32 Function(Pointer<Utf8>);
typedef _Unsetenv = int Function(Pointer<Utf8>);

const _sampleS16le = 3;
const _playback = 1;
const _record = 2;
const _bytesPerFrame = samplesPerFrame * 2;

class _Pulse {
  _Pulse(DynamicLibrary l)
    : open = l.lookupFunction<_NewC, _New>('pa_simple_new'),
      read = l.lookupFunction<_IoC, _Io>('pa_simple_read'),
      write = l.lookupFunction<_IoC, _Io>('pa_simple_write'),
      drain = l.lookupFunction<_DrainC, _Drain>('pa_simple_drain'),
      free = l.lookupFunction<_FreeC, _Free>('pa_simple_free'),
      strerror = l.lookupFunction<_StrerrorC, _Strerror>('pa_strerror');
  final _New open;
  final _Io read, write;
  final _Drain drain;
  final _Free free;
  final _Strerror strerror;

  static _Pulse load() {
    try {
      return _Pulse(DynamicLibrary.open('libpulse-simple.so.0'));
    } on ArgumentError catch (e) {
      throw AudioUnavailable('libpulse-simple.so.0: $e (sudo apt install libpulse0)');
    }
  }

  /// A stream, with `props` (PULSE_PROP) set while it is made: the
  /// PulseAudio route to its echo canceller.
  Pointer<Void> connect(int direction, String? device, String streamName, String? props) {
    final libc = DynamicLibrary.process();
    final setenv = libc.lookupFunction<_SetenvC, _Setenv>('setenv');
    final unsetenv = libc.lookupFunction<_UnsetenvC, _Unsetenv>('unsetenv');
    final arena = Arena();
    try {
      final spec = arena<_SampleSpec>()
        ..ref.format = _sampleS16le
        ..ref.rate = sampleRate
        ..ref.channels = 1;
      final attr = arena<_BufferAttr>()
        ..ref.maxlength = 0xFFFFFFFF
        ..ref.tlength = direction == _playback ? _bytesPerFrame * 3 : 0xFFFFFFFF
        ..ref.prebuf = 0xFFFFFFFF
        ..ref.minreq = 0xFFFFFFFF
        ..ref.fragsize = direction == _record ? _bytesPerFrame : 0xFFFFFFFF;
      final err = arena<Int32>();
      final key = 'PULSE_PROP'.toNativeUtf8(allocator: arena);
      if (props != null) setenv(key, props.toNativeUtf8(allocator: arena), 1);
      final s = open(
        nullptr,
        'Thistle Chat'.toNativeUtf8(allocator: arena),
        direction,
        device == null ? nullptr : device.toNativeUtf8(allocator: arena),
        streamName.toNativeUtf8(allocator: arena),
        spec,
        nullptr,
        attr,
        err,
      );
      if (props != null) unsetenv(key);
      if (s == nullptr) {
        throw AudioUnavailable('${device ?? 'the default device'}: ${strerror(err.value).toDartString()}');
      }
      return s;
    } finally {
      arena.releaseAll();
    }
  }
}

typedef _CaptureArgs = ({SendPort port, String? device, String name, String? props, int stopFlag});

void _capture(_CaptureArgs a) {
  final Pointer<Void> s;
  final _Pulse pa;
  try {
    pa = _Pulse.load();
    s = pa.connect(_record, a.device, a.name, a.props);
  } on AudioUnavailable catch (e) {
    a.port.send(e.why);
    return;
  }
  a.port.send(true);
  final stop = Pointer<Int32>.fromAddress(a.stopFlag);
  final buf = calloc<Uint8>(_bytesPerFrame);
  final err = calloc<Int32>();
  while (stop.value == 0) {
    if (pa.read(s, buf.cast(), _bytesPerFrame, err) < 0) {
      a.port.send(pa.strerror(err.value).toDartString());
      break;
    }
    a.port.send(TransferableTypedData.fromList([Uint8List.fromList(buf.asTypedList(_bytesPerFrame))]));
  }
  pa.free(s);
  calloc.free(buf);
  calloc.free(err);
  a.port.send(null);
}

/// A microphone: 20 ms frames until the subscription is cancelled.
/// `props` are PulseAudio stream properties, such as asking for its echo
/// canceller.
Future<Stream<Int16List>> pulseMicrophone({String? device, String name = 'microphone', String? props}) async {
  final stopFlag = calloc<Int32>();
  final port = ReceivePort();
  final ready = Completer<void>();
  final out = StreamController<Int16List>();
  final done = Completer<void>();
  port.listen((m) {
    if (m == true) {
      ready.complete();
    } else if (m is String) {
      ready.isCompleted ? out.addError(AudioUnavailable(m)) : ready.completeError(AudioUnavailable(m));
    } else if (m is TransferableTypedData) {
      if (!out.isClosed) out.add(m.materialize().asInt16List());
    } else if (m == null) {
      port.close();
      calloc.free(stopFlag);
      done.complete();
    }
  });
  await Isolate.spawn(_capture, (
    port: port.sendPort,
    device: device,
    name: name,
    props: props,
    stopFlag: stopFlag.address,
  ), errorsAreFatal: true);
  try {
    await ready.future;
  } on AudioUnavailable {
    port.close();
    calloc.free(stopFlag);
    rethrow;
  }
  out.onCancel = () async {
    stopFlag.value = 1;
    await done.future;
    await out.close();
  };
  return out.stream;
}

typedef _PlayArgs = ({SendPort port, String? device, String name, String? props});

void _play(_PlayArgs a) {
  final Pointer<Void> s;
  final _Pulse pa;
  try {
    pa = _Pulse.load();
    s = pa.connect(_playback, a.device, a.name, a.props);
  } on AudioUnavailable catch (e) {
    a.port.send(e.why);
    return;
  }
  final inbox = ReceivePort();
  a.port.send(inbox.sendPort);
  final buf = calloc<Uint8>(_bytesPerFrame * 4);
  final err = calloc<Int32>();
  inbox.listen((m) {
    if (m is TransferableTypedData) {
      final bytes = m.materialize().asUint8List();
      for (var at = 0; at < bytes.length; at += _bytesPerFrame * 4) {
        final n = (bytes.length - at).clamp(0, _bytesPerFrame * 4);
        buf.asTypedList(n).setAll(0, Uint8List.sublistView(bytes, at, at + n));
        pa.write(s, buf.cast(), n, err);
      }
    } else {
      pa.drain(s, err);
      pa.free(s);
      calloc.free(buf);
      calloc.free(err);
      inbox.close();
      a.port.send(null);
    }
  });
}

class _PulseSpeaker implements AudioOut {
  _PulseSpeaker(this._to, this._done);
  final SendPort _to;
  final Future<void> _done;
  bool _closed = false;

  @override
  void write(Int16List pcm) {
    if (_closed) return;
    _to.send(TransferableTypedData.fromList([pcm.buffer.asUint8List(pcm.offsetInBytes, pcm.lengthInBytes)]));
  }

  @override
  Future<void> close() async {
    if (_closed) return;
    _closed = true;
    _to.send('close');
    await _done;
  }
}

/// A speaker; what is written plays as it comes, about 60 ms behind.
Future<AudioOut> pulseSpeaker({String? device, String name = 'speaker', String? props}) async {
  final port = ReceivePort();
  final ready = Completer<SendPort>();
  final done = Completer<void>();
  port.listen((m) {
    if (m is SendPort) {
      ready.complete(m);
    } else if (m is String) {
      ready.completeError(AudioUnavailable(m));
      port.close();
    } else if (m == null) {
      port.close();
      done.complete();
    }
  });
  await Isolate.spawn(_play, (port: port.sendPort, device: device, name: name, props: props));
  return _PulseSpeaker(await ready.future, done.future);
}
