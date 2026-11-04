/// Ogg/Opus files (RFC 3533, RFC 7845), as voice messages carry them:
/// one logical stream, mono, recorded at 16 kHz, one 20 ms Opus packet
/// after another. Every page's CRC is checked on reading.
library;

import 'dart:convert';
import 'dart:typed_data';

/// What a voice message holds: its header and its audio packets.
class OggOpus {
  OggOpus({
    required this.channels,
    required this.inputRate,
    required this.preSkip,
    required this.lastGranule,
    required this.packets,
  });
  final int channels;
  final int inputRate;

  /// Samples at 48 kHz to drop from the start of the decoded audio.
  final int preSkip;
  final int lastGranule;
  final List<Uint8List> packets;

  /// The audio's length, from the last page's granule position.
  int get durationMs => (lastGranule - preSkip) * 1000 ~/ 48000;
}

final List<int> _crcTable = List.generate(256, (i) {
  var r = i << 24;
  for (var k = 0; k < 8; k++) {
    r = (r & 0x80000000) != 0 ? ((r << 1) ^ 0x04C11DB7) : (r << 1);
    r &= 0xFFFFFFFF;
  }
  return r;
});

int _crc(Uint8List data) {
  var r = 0;
  for (final b in data) {
    r = ((r << 8) & 0xFFFFFFFF) ^ _crcTable[((r >> 24) & 0xFF) ^ b];
  }
  return r;
}

/// The Opus packets of an Ogg/Opus file; FormatException if it is not one.
OggOpus readOggOpus(List<int> bytes) {
  final data = bytes is Uint8List ? bytes : Uint8List.fromList(bytes);
  final view = ByteData.sublistView(data);
  final packets = <Uint8List>[];
  final partial = BytesBuilder(copy: false);
  var lastGranule = 0;
  var pos = 0;
  int? serial;
  while (pos < data.length) {
    if (data.length - pos < 27 || ascii.decode(data.sublist(pos, pos + 4), allowInvalid: true) != 'OggS') {
      throw FormatException('no Ogg page at byte $pos');
    }
    if (data[pos + 4] != 0) throw const FormatException('Ogg version is not 0');
    final granule = view.getInt64(pos + 6, Endian.little);
    final pageSerial = view.getUint32(pos + 14, Endian.little);
    final checksum = view.getUint32(pos + 22, Endian.little);
    final nseg = data[pos + 26];
    final bodyAt = pos + 27 + nseg;
    if (bodyAt > data.length) throw FormatException('truncated Ogg page at byte $pos');
    final lacing = data.sublist(pos + 27, bodyAt);
    final end = bodyAt + lacing.fold<int>(0, (a, b) => a + b);
    if (end > data.length) throw FormatException('truncated Ogg page at byte $pos');
    final page = Uint8List.fromList(data.sublist(pos, end));
    page.fillRange(22, 26, 0);
    if (_crc(page) != checksum) throw FormatException('bad CRC on the Ogg page at byte $pos');
    if (serial != null && pageSerial != serial) throw const FormatException('more than one Ogg stream');
    serial = pageSerial;
    if (granule >= 0) lastGranule = granule;
    var at = bodyAt, run = 0;
    for (final n in lacing) {
      run += n;
      if (n < 255) {
        partial.add(data.sublist(at, at + run));
        packets.add(partial.takeBytes());
        at += run;
        run = 0;
      }
    }
    if (run > 0) partial.add(data.sublist(at, at + run));
    pos = end;
  }
  if (partial.isNotEmpty) throw const FormatException('the last Ogg packet is not complete');
  bool starts(Uint8List p, String magic) => p.length >= 8 && ascii.decode(p.sublist(0, 8), allowInvalid: true) == magic;
  if (packets.length < 2 || !starts(packets[0], 'OpusHead') || !starts(packets[1], 'OpusTags')) {
    throw const FormatException('not an Ogg/Opus file');
  }
  final head = ByteData.sublistView(packets[0]);
  if (packets[0].length < 19) throw const FormatException('OpusHead too short');
  return OggOpus(
    channels: packets[0][9],
    preSkip: head.getUint16(10, Endian.little),
    inputRate: head.getUint32(12, Endian.little),
    lastGranule: lastGranule,
    packets: packets.sublist(2),
  );
}

/// Samples at 48 kHz per 20 ms packet: the granule's unit.
const _granulePerPacket = 960;

/// An Ogg/Opus file of 20 ms mono packets, as a voice message sends it.
/// `preSkip` is the encoder's lookahead at 48 kHz (libopus: 312 at 16 kHz);
/// `ms`, the audio's length, trims the end (the encoder's last packet
/// carries the lookahead's worth past it), at most what the packets hold.
Uint8List writeOggOpus(
  List<List<int>> packets, {
  int? ms,
  int inputRate = 16000,
  int preSkip = 312,
  int serial = 1,
  String vendor = 'thistle-chat',
}) {
  final out = BytesBuilder(copy: false);
  var sequence = 0;

  void page(List<List<int>> body, int granule, int flags) {
    final lacing = <int>[];
    for (final p in body) {
      var n = p.length;
      while (n >= 255) {
        lacing.add(255);
        n -= 255;
      }
      lacing.add(n);
    }
    final header = ByteData(27 + lacing.length);
    for (var i = 0; i < 4; i++) {
      header.setUint8(i, 'OggS'.codeUnitAt(i));
    }
    header.setUint8(5, flags);
    header.setInt64(6, granule, Endian.little);
    header.setUint32(14, serial, Endian.little);
    header.setUint32(18, sequence++, Endian.little);
    header.setUint8(26, lacing.length);
    for (var i = 0; i < lacing.length; i++) {
      header.setUint8(27 + i, lacing[i]);
    }
    final b = BytesBuilder(copy: false)..add(header.buffer.asUint8List());
    for (final p in body) {
      b.add(p);
    }
    final bytes = b.takeBytes();
    ByteData.sublistView(bytes).setUint32(22, _crc(bytes), Endian.little);
    out.add(bytes);
  }

  final head = ByteData(19);
  for (var i = 0; i < 8; i++) {
    head.setUint8(i, 'OpusHead'.codeUnitAt(i));
  }
  head
    ..setUint8(8, 1)
    ..setUint8(9, 1)
    ..setUint16(10, preSkip, Endian.little)
    ..setUint32(12, inputRate, Endian.little)
    ..setInt16(16, 0, Endian.little)
    ..setUint8(18, 0);
  page([head.buffer.asUint8List()], 0, 0x02);

  final v = utf8.encode(vendor);
  final tags = BytesBuilder()
    ..add(ascii.encode('OpusTags'))
    ..add((ByteData(4)..setUint32(0, v.length, Endian.little)).buffer.asUint8List())
    ..add(v)
    ..add(Uint8List(4)); // no comments
  page([tags.takeBytes()], 0, 0);

  // About a second a page; a page holds at most 255 lacing values.
  var granule = 0;
  final body = <List<int>>[];
  var segments = 0;
  for (var i = 0; i < packets.length; i++) {
    final p = packets[i];
    final need = p.length ~/ 255 + 1;
    if (body.isNotEmpty && (segments + need > 255 || body.length >= 50)) {
      page(body, granule, 0);
      body.clear();
      segments = 0;
    }
    body.add(p);
    segments += need;
    granule += _granulePerPacket;
  }
  // The last page's granule ends the audio: pre-skip plus its length, no
  // more than what the packets decode to.
  final end = ms == null ? granule : (preSkip + ms * 48).clamp(0, granule);
  page(body, end, 0x04);
  return out.takeBytes();
}
