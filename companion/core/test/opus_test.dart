import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:test/test.dart';

import 'vectors.dart';

/// A tone at 16 kHz, as a microphone would give it.
Int16List tone(int ms, {double hz = 440, double amplitude = 8000}) {
  final n = 16 * ms;
  return Int16List.fromList([for (var i = 0; i < n; i++) (amplitude * sin(2 * pi * hz * i / 16000)).round()]);
}

double rms(List<int> s) => sqrt(s.fold<double>(0, (a, x) => a + x * x) / s.length);

/// The frequency of a clean tone, from its zero crossings.
double frequency(List<int> s) {
  var crossings = 0;
  for (var i = 1; i < s.length; i++) {
    if ((s[i - 1] < 0) != (s[i] < 0)) crossings++;
  }
  return crossings / 2 / (s.length / 16000);
}

void main() {
  test('a tone encoded and decoded keeps its length, its level and its pitch', () {
    final enc = OpusEncoder();
    final dec = OpusDecoder();
    addTearDown(enc.dispose);
    addTearDown(dec.dispose);
    final pcm = tone(1000);
    final out = <int>[];
    for (var at = 0; at < pcm.length; at += samplesPerFrame) {
      final packet = enc.encode(Int16List.sublistView(pcm, at, at + samplesPerFrame));
      expect(packet.length, inInclusiveRange(1, 400));
      out.addAll(dec.decode(packet));
    }
    expect(out.length, pcm.length);
    final steady = out.sublist(1600); // past the codec's start
    expect(rms(steady) / rms(pcm), inInclusiveRange(0.7, 1.4));
    expect(frequency(steady), closeTo(440, 15));
  });

  test("the encoder's lookahead is the vector's pre-skip", () {
    final enc = OpusEncoder();
    addTearDown(enc.dispose);
    expect(enc.preSkip, vector('voice.json')['pre_skip']);
  });

  test("the vector's packets decode to its 440 Hz tone, 20 ms each", () {
    final dec = OpusDecoder();
    addTearDown(dec.dispose);
    final file = readOggOpus(File.fromUri(vectorsDir.resolve('voice.opus')).readAsBytesSync());
    final out = <int>[];
    for (final p in file.packets) {
      final pcm = dec.decode(p);
      expect(pcm.length, samplesPerFrame);
      out.addAll(pcm);
    }
    expect(frequency(out.sublist(1600, 16000)), closeTo(440, 15));
    expect(rms(out.sublist(1600, 16000)), greaterThan(1000));
  });

  test('a lost packet is concealed with 20 ms of audio', () {
    final enc = OpusEncoder();
    final dec = OpusDecoder();
    addTearDown(enc.dispose);
    addTearDown(dec.dispose);
    final pcm = tone(200);
    for (var at = 0; at < pcm.length; at += samplesPerFrame) {
      dec.decode(enc.encode(Int16List.sublistView(pcm, at, at + samplesPerFrame)));
    }
    final concealed = dec.decode(null);
    expect(concealed.length, samplesPerFrame);
    expect(rms(concealed), greaterThan(100));
  });

  test('a recording becomes an Ogg/Opus voice message of its length', () {
    final rec = VoiceRecorder();
    final pcm = tone(1500);
    for (var at = 0; at < pcm.length; at += 160) {
      rec.add(Int16List.sublistView(pcm, at, at + 160)); // the microphone's chunks need not be frames
    }
    final msg = rec.finish();
    expect(msg.ms, 1500);
    final file = readOggOpus(msg.bytes);
    expect(file.durationMs, 1500);
    expect(file.inputRate, 16000);
    expect(file.channels, 1);
    expect(decodeVoice(msg.bytes).length, 1500 * 16);
  });

  test('a recording stops taking audio at two minutes', () {
    final rec = VoiceRecorder();
    final second = Int16List(16000);
    for (var i = 0; i < 125; i++) {
      rec.add(second);
    }
    expect(rec.full, isTrue);
    expect(rec.finish().ms, maxVoiceMs);
  });
}
