import 'dart:io';
import 'dart:typed_data';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:test/test.dart';

import 'vectors.dart';

void main() {
  final v = vector('voice.json');
  final file = File.fromUri(vectorsDir.resolve('voice.opus')).readAsBytesSync();
  final frames = [for (final f in v['frames'] as List) fromHex(f['hex'] as String)];

  group('reading the voice vector', () {
    test('gives its header and every Opus packet, in order', () {
      final o = readOggOpus(file);
      expect(o.channels, v['channels']);
      expect(o.inputRate, v['input_rate']);
      expect(o.preSkip, v['pre_skip']);
      expect(o.packets.length, frames.length);
      for (var i = 0; i < frames.length; i++) {
        expect(o.packets[i], frames[i], reason: 'packet $i');
      }
    });

    test('gives its length in milliseconds', () {
      expect(readOggOpus(file).durationMs, v['ms']);
    });

    test('refuses a page whose CRC does not match', () {
      final bad = Uint8List.fromList(file);
      bad[bad.length - 1] ^= 0xff;
      expect(() => readOggOpus(bad), throwsFormatException);
    });

    test('refuses what is not Ogg/Opus', () {
      expect(() => readOggOpus(Uint8List.fromList('RIFF....WAVEfmt '.codeUnits)), throwsFormatException);
    });
  });

  group('writing', () {
    test("the vector's packets read back as they went in, with the same length", () {
      final o = readOggOpus(writeOggOpus(frames, ms: v['ms'] as int));
      expect(o.packets, frames);
      expect(o.channels, 1);
      expect(o.inputRate, 16000);
      expect(o.durationMs, v['ms']);
    });

    test('a two-minute message spans several pages and reads back whole', () {
      // 6000 packets of audio and one more carrying the encoder's lookahead.
      final many = [
        for (var i = 0; i < 6001; i++) Uint8List.fromList([0x48, i & 0xff, i >> 8]),
      ];
      final o = readOggOpus(writeOggOpus(many, ms: 120000));
      expect(o.packets.length, 6001);
      expect(o.packets[6000], many[6000]);
      expect(o.durationMs, 120000);
    });

    test('with no length given, the audio is every packet less the lookahead', () {
      expect(readOggOpus(writeOggOpus(frames)).durationMs, (frames.length * 960 - 312) * 1000 ~/ 48000);
    });

    test("the reference peer's Python reader agrees", () async {
      final dir = Directory.systemTemp.createTempSync('ogg');
      addTearDown(() => dir.deleteSync(recursive: true));
      final tmp = File('${dir.path}/w.opus')..writeAsBytesSync(writeOggOpus(frames, ms: v['ms'] as int));
      final r = await Process.run('python3', [
        '-c',
        'import sys; sys.path.insert(0, "../../tools"); from tailnet_chat import ogg; '
            'o = ogg.read_opus(open(sys.argv[1], "rb").read()); print(len(o.frames), o.duration_ms, o.input_rate)',
        tmp.path,
      ]);
      expect(r.stdout.toString().trim(), '${frames.length} ${v['ms']} 16000', reason: '${r.stderr}');
    });
  });
}
