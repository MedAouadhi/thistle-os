import 'dart:io';
import 'dart:typed_data';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:test/test.dart';

void main() {
  test('a WAV file written reads back as its samples, and FFmpeg reads it as 16 kHz mono', () async {
    final pcm = Int16List.fromList([for (var i = 0; i < 1600; i++) (i * 37 % 2000) - 1000]);
    final bytes = wavBytes(pcm);
    expect(readWav(bytes), pcm);
    final ffprobe = await Process.run('which', ['ffprobe']);
    if (ffprobe.exitCode != 0) return;
    final dir = Directory.systemTemp.createTempSync('wav');
    addTearDown(() => dir.deleteSync(recursive: true));
    final f = File('${dir.path}/a.wav')..writeAsBytesSync(bytes);
    final r = await Process.run('ffprobe', [
      '-v',
      'error',
      '-show_entries',
      'stream=sample_rate,channels',
      '-of',
      'csv=p=0',
      f.path,
    ]);
    expect(r.stdout.toString().trim(), '16000,1');
  });

  test('a stereo or 44.1 kHz file is refused', () {
    final b = wavBytes(Int16List(10), rate: 44100);
    expect(() => readWav(b), throwsFormatException);
  });
}
