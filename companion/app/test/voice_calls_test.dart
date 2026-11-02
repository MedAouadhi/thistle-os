import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/permissions.dart';
import 'package:thistle_chat/src/tailnet.dart';

import 'helpers.dart';

/// A chat peer standing for the device or another laptop.
Future<ChatPeer> otherPeer() async {
  final p = ChatPeer(
    identity: Identity.generate(),
    store: Store.open(':memory:'),
    name: 'thistle-46cf20',
    port: 0,
    loopback: true,
  );
  await p.start();
  return p;
}

double rms(List<int> s) => s.isEmpty ? 0 : sqrt(s.fold<double>(0, (a, x) => a + x * x) / s.length);

double frequency(List<int> s) {
  var crossings = 0;
  for (var i = 1; i < s.length; i++) {
    if ((s[i - 1] < 0) != (s[i] < 0)) crossings++;
  }
  return crossings / 2 / (s.length / sampleRate);
}

void main() {
  late TestTailnet tailnet;
  late RecordingNotifier notifier;
  late FakeAudio audio;
  late ChatModel app;
  late ChatPeer other;
  late String box;

  Future<void> befriend() async {
    other.addContact(app.listening!, name: 'laptop');
    other.sendText('laptop', 'hi');
    await eventually(() => app.requests.isNotEmpty);
    app.accept(other.identity.keyHex);
    await eventually(() => other.contact('laptop')!.acceptedByThem == true);
    box = other.identity.keyHex;
  }

  setUp(() async {
    tailnet = TestTailnet(parseTailscaleStatus(File('test/fixtures/tailscale_status_running.json').readAsStringSync()));
    notifier = RecordingNotifier();
    audio = FakeAudio();
    app = testModel(tailnet, notifier, audio: audio);
    other = await otherPeer();
    await app.start();
    await befriend();
  });

  tearDown(() async {
    await app.close();
    await other.stop();
  });

  group('voice messages', () {
    test('a recording from the microphone reaches the other side as Ogg/Opus of its length', () async {
      expect(await app.talk.startRecording(box), isNull);
      expect(app.talk.recordingBox, box);
      await Future<void>.delayed(const Duration(milliseconds: 1200));
      expect(app.talk.recordingMs, greaterThan(900));
      await app.talk.stopRecording();
      final got = await eventually(
        () => other.messages('laptop').where((m) => m.voice != null).isNotEmpty,
      ).then((_) => other.messages('laptop').firstWhere((m) => m.voice != null));
      final file = readOggOpus(got.voice!);
      expect(file.inputRate, 16000);
      expect(file.channels, 1);
      expect(file.durationMs, got.ms);
      expect(got.ms, inInclusiveRange(1000, 1600));
      expect(frequency(decodeVoice(got.voice!).sublist(1600)), closeTo(440, 20));
      await eventually(() => app.waiting(box).isEmpty);
    });

    test('a recording cancelled sends nothing', () async {
      await app.talk.startRecording(box);
      await Future<void>.delayed(const Duration(milliseconds: 300));
      await app.talk.stopRecording(send: false);
      expect(app.talk.recordingBox, isNull);
      await Future<void>.delayed(const Duration(milliseconds: 300));
      expect(other.messages('laptop').where((m) => m.voice != null), isEmpty);
    });

    test('a received voice message plays only when asked, the whole of it', () async {
      final data = File('../../docs/tailnet-chat/vectors/voice.opus').readAsBytesSync();
      final id = other.sendVoice('laptop', data, 1500);
      await eventually(() => app.messages(box).any((m) => m.id == id));
      expect(audio.played, isEmpty, reason: 'nothing plays by surprise');
      final m = app.messages(box).firstWhere((m) => m.id == id);
      await app.talk.play(m);
      expect(app.talk.playingId, id);
      await eventually(() => app.talk.playingId == null, timeout: const Duration(seconds: 4));
      final pcm = audio.played.single;
      expect(pcm.length, closeTo(1500 * 16, 320));
      expect(frequency(pcm.sublist(1600)), closeTo(440, 20));
    });

    test('a microphone refused on Android stops the recording, and says so', () async {
      final perms = FakePermissions()..answer = PermissionAnswer.denied;
      final android = testModel(tailnet, notifier, audio: audio, permissions: perms);
      addTearDown(android.close);
      await android.start();
      final why = await android.talk.startRecording(box);
      expect(why, contains('microphone'));
      expect(android.talk.recordingBox, isNull);
      expect(perms.asked, [AppPermission.microphone]);
    });
  });

  group('calls', () {
    test('a call placed is answered, carries the microphone both ways, and hangs up', () async {
      final heard = <int>[];
      other.callAudio.listen((a) => heard.add(a.seq));
      final id = (await app.talk.startCall(box))!;
      await eventually(() => app.talk.call?.state == 'connected');
      expect(app.talk.echoCancellation, 'none (test)');
      final media = CallMedia(other, id, mic: pacedAudio(toneFrame), speaker: NoAudioOut())..start();
      await eventually(() => heard.length > 20 && audio.callOut.length > 8000, timeout: const Duration(seconds: 4));
      expect(frequency(audio.callOut.sublist(3200)), closeTo(440, 20));
      app.talk.hangUp();
      await eventually(() => other.calls[id]!.state == 'ended:hangup');
      await eventually(() => audio.callClosed);
      await media.stop();
    });

    test('muted, the other side hears silence and the call stays up', () async {
      final id = (await app.talk.startCall(box))!;
      await eventually(() => app.talk.call?.state == 'connected');
      app.talk.muted = true;
      final heard = <int>[];
      final media = CallMedia(other, id, mic: pacedAudio((_) => Int16List(samplesPerFrame)), speaker: _Into(heard))
        ..start();
      await Future<void>.delayed(const Duration(milliseconds: 800));
      expect(other.calls[id]!.state, 'connected');
      expect(rms(heard), lessThan(100));
      await media.stop();
      app.talk.hangUp();
    });

    test('a call in the background rings with a notification; answered, it connects', () async {
      app.foreground = false;
      other.call('laptop');
      await eventually(() => notifier.rang.isNotEmpty);
      expect(notifier.rang.single, 'thistle-46cf20');
      expect(app.talk.call?.state, 'ringing');
      expect(app.talk.incoming, isTrue);
      await eventually(() => audio.rings > 0); // the ring plays
      await app.talk.answer();
      final id = app.talk.callId!;
      await eventually(() => other.calls[id]!.state == 'connected');
      expect(notifier.stoppedRinging, contains(id));
      app.talk.hangUp();
    });

    test('declined, the caller hears `declined`', () async {
      final id = await other.call('laptop');
      await eventually(() => app.talk.call?.state == 'ringing');
      app.talk.decline();
      await eventually(() => other.calls[id]!.state == 'declined:declined');
    });

    test('a call that rings out unanswered is a missed call', () async {
      app.foreground = false;
      final id = await other.call('laptop');
      await eventually(() => app.talk.call?.state == 'ringing');
      other.endCall(id);
      await eventually(() => notifier.shown.any((s) => s.title.contains('Missed call')));
      expect(notifier.shown.last.body, contains('thistle-46cf20'));
    });

    test('a call to a contact who is away fails at once as unreachable', () async {
      await other.stop();
      final id = await app.talk.startCall(box);
      expect(app.talk.call?.state, 'ended:unreachable');
      expect(id, isNotNull);
      other = await otherPeer(); // for tearDown
    });
  });
}

Int16List toneFrame([int i = 0]) => Int16List.fromList([
  for (var k = 0; k < samplesPerFrame; k++) (8000 * sin(2 * pi * 440 * (i * samplesPerFrame + k) / sampleRate)).round(),
]);

class _Into implements AudioOut {
  _Into(this.into);
  final List<int> into;
  @override
  void write(Int16List pcm) => into.addAll(pcm);
  @override
  Future<void> close() async {}
}
