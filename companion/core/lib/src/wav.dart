/// 16-bit PCM WAV files, mono at 16 kHz: a file for a microphone or a
/// speaker in tests, the headless core and the app's `--mic`/`--speaker`.
library;

import 'dart:io';
import 'dart:typed_data';

import 'call_media.dart';
import 'opus.dart' show sampleRate;

Uint8List wavBytes(Int16List pcm, {int rate = sampleRate}) {
  final h = ByteData(44);
  void tag(int at, String s) {
    for (var i = 0; i < 4; i++) {
      h.setUint8(at + i, s.codeUnitAt(i));
    }
  }

  tag(0, 'RIFF');
  h.setUint32(4, 36 + pcm.lengthInBytes, Endian.little);
  tag(8, 'WAVE');
  tag(12, 'fmt ');
  h
    ..setUint32(16, 16, Endian.little)
    ..setUint16(20, 1, Endian.little)
    ..setUint16(22, 1, Endian.little)
    ..setUint32(24, rate, Endian.little)
    ..setUint32(28, rate * 2, Endian.little)
    ..setUint16(32, 2, Endian.little)
    ..setUint16(34, 16, Endian.little);
  tag(36, 'data');
  h.setUint32(40, pcm.lengthInBytes, Endian.little);
  return (BytesBuilder(copy: false)
        ..add(h.buffer.asUint8List())
        ..add(pcm.buffer.asUint8List(pcm.offsetInBytes, pcm.lengthInBytes)))
      .takeBytes();
}

/// The samples of a mono 16-bit WAV file at 16 kHz.
Int16List readWav(List<int> bytes) {
  final data = bytes is Uint8List ? bytes : Uint8List.fromList(bytes);
  final v = ByteData.sublistView(data);
  String at(int i) => String.fromCharCodes(data.sublist(i, i + 4));
  if (data.length < 12 || at(0) != 'RIFF' || at(8) != 'WAVE') throw const FormatException('not a WAV file');
  var pos = 12;
  int? channels, rate, bits;
  while (pos + 8 <= data.length) {
    final id = at(pos);
    final size = v.getUint32(pos + 4, Endian.little);
    if (id == 'fmt ') {
      channels = v.getUint16(pos + 10, Endian.little);
      rate = v.getUint32(pos + 12, Endian.little);
      bits = v.getUint16(pos + 22, Endian.little);
    } else if (id == 'data') {
      if (channels != 1 || rate != sampleRate || bits != 16) {
        throw FormatException('want mono 16-bit at $sampleRate Hz, not $channels ch $bits-bit at $rate Hz');
      }
      final end = (pos + 8 + size).clamp(0, data.length);
      return Int16List.fromList(Int16List.sublistView(Uint8List.fromList(data.sublist(pos + 8, end))));
    }
    pos += 8 + size + (size & 1);
  }
  throw const FormatException('no data in the WAV file');
}

/// A speaker that keeps what it plays, written to `path` as WAV on close.
class WavFileOut implements AudioOut {
  WavFileOut(this.path);
  final String path;
  final BytesBuilder _pcm = BytesBuilder();
  @override
  void write(Int16List pcm) => _pcm.add(pcm.buffer.asUint8List(pcm.offsetInBytes, pcm.lengthInBytes));
  @override
  Future<void> close() async {
    final bytes = _pcm.takeBytes();
    await File(path).writeAsBytes(wavBytes(Int16List.sublistView(bytes)));
  }
}

/// A microphone that plays a WAV file's samples, over and over, in real time.
Stream<Int16List> wavFileIn(String path) {
  final pcm = readWav(File(path).readAsBytesSync());
  if (pcm.isEmpty) return pacedAudio((_) => Int16List(320));
  return pacedAudio((i) {
    final out = Int16List(320);
    for (var k = 0; k < 320; k++) {
      out[k] = pcm[(i * 320 + k) % pcm.length];
    }
    return out;
  });
}
