import 'dart:math';
import 'dart:typed_data';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:test/test.dart';

import 'peer_test.dart' show befriend, eventually, startPeer;

Int16List toneFrame(int index, {double hz = 440}) => Int16List.fromList([
  for (var i = 0; i < samplesPerFrame; i++)
    (8000 * sin(2 * pi * hz * (index * samplesPerFrame + i) / sampleRate)).round(),
]);

double rms(List<int> s) => s.isEmpty ? 0 : sqrt(s.fold<double>(0, (a, x) => a + x * x) / s.length);

double frequency(List<int> s) {
  var crossings = 0;
  for (var i = 1; i < s.length; i++) {
    if ((s[i - 1] < 0) != (s[i] < 0)) crossings++;
  }
  return crossings / 2 / (s.length / sampleRate);
}

class Recorded implements AudioOut {
  final List<int> samples = [];
  @override
  void write(Int16List pcm) => samples.addAll(pcm);
  @override
  Future<void> close() async {}
}

void main() {
  late ChatPeer alice, bob;
  setUp(() async {
    alice = await startPeer('alice');
    bob = await startPeer('bob');
    await befriend(alice, bob);
  });
  tearDown(() async {
    await alice.stop();
    await bob.stop();
  });

  test("a call carries one side's microphone to the other's speaker, both ways at once", () async {
    final aliceHears = Recorded(), bobHears = Recorded();
    final call = await alice.call('bob');
    await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
    final a = CallMedia(alice, call, mic: pacedAudio(toneFrame), speaker: aliceHears)..start();
    final b = CallMedia(bob, call, mic: pacedAudio((i) => toneFrame(i, hz: 1000)), speaker: bobHears)..start();
    await Future<void>.delayed(const Duration(milliseconds: 1500));
    await a.stop();
    await b.stop();
    // Past the jitter buffer's fill and the codec's start.
    final heardByBob = bobHears.samples.sublist(sampleRate ~/ 4);
    final heardByAlice = aliceHears.samples.sublist(sampleRate ~/ 4);
    expect(heardByBob.length, greaterThan(sampleRate ~/ 2));
    expect(frequency(heardByBob), closeTo(440, 20));
    expect(frequency(heardByAlice), closeTo(1000, 30));
    expect(rms(heardByBob), greaterThan(2000));
  });

  test('muted, the other side hears silence but the call stays up', () async {
    final bobHears = Recorded();
    final call = await alice.call('bob');
    await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
    final a = CallMedia(alice, call, mic: pacedAudio(toneFrame), speaker: Recorded())
      ..muted = true
      ..start();
    final b = CallMedia(bob, call, mic: pacedAudio((_) => Int16List(samplesPerFrame)), speaker: bobHears)..start();
    await Future<void>.delayed(const Duration(milliseconds: 800));
    await a.stop();
    await b.stop();
    expect(bob.calls[call]!.frames, greaterThan(20));
    expect(rms(bobHears.samples), lessThan(100));
    expect(bob.calls[call]!.state, 'connected');
  });

  test('the volume scales what is played', () async {
    final loud = Recorded(), quiet = Recorded();
    final call = await alice.call('bob');
    await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
    final a = CallMedia(alice, call, mic: pacedAudio(toneFrame), speaker: Recorded())..start();
    final b = CallMedia(bob, call, mic: pacedAudio((_) => Int16List(samplesPerFrame)), speaker: loud)..start();
    await Future<void>.delayed(const Duration(milliseconds: 700));
    await b.stop();
    final b2 = CallMedia(bob, call, mic: pacedAudio((_) => Int16List(samplesPerFrame)), speaker: quiet)
      ..volume = 0.25
      ..start();
    await Future<void>.delayed(const Duration(milliseconds: 700));
    await b2.stop();
    await a.stop();
    final r = rms(quiet.samples.sublist(sampleRate ~/ 5)) / rms(loud.samples.sublist(sampleRate ~/ 5));
    expect(r, closeTo(0.25, 0.08));
  });

  test('media stops by itself when the call ends', () async {
    final call = await alice.call('bob');
    await eventually(() => alice.calls[call]!.state == 'connected', what: 'the answer');
    final a = CallMedia(alice, call, mic: pacedAudio(toneFrame), speaker: Recorded())..start();
    alice.endCall(call);
    await eventually(() => a.stopped, what: 'the media to stop');
  });
}
