import 'dart:io';
import 'dart:math';
import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/call_engine.dart';
import 'package:thistle_chat/src/chat_model.dart';
import 'package:thistle_chat/src/permissions.dart';
import 'package:thistle_chat/src/tailnet.dart';
import 'package:thistle_chat/src/ui/call_page.dart';

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

    group('on Android, where the phone rings', () {
      setUp(() async {
        await app.close();
        await other.stop();
        notifier = RecordingNotifier(ringsItself: true);
        app = testModel(tailnet, notifier, audio: audio);
        other = await otherPeer();
        await app.start();
        await befriend();
      });

      test('a call in the background asks the phone to ring with the caller, and the app plays no ring', () async {
        app.foreground = false;
        await other.call('laptop');
        await eventually(() => notifier.rang.isNotEmpty);
        expect(notifier.rang.single, 'thistle-46cf20');
        expect(notifier.rangInFront.single, isFalse);
        await Future<void>.delayed(const Duration(milliseconds: 200));
        expect(audio.rings, 0);
      });

      test('with the app in front, the phone rings and the call screen is the notification', () async {
        await other.call('laptop');
        await eventually(() => notifier.rang.isNotEmpty);
        expect(notifier.rangInFront.single, isTrue);
      });

      Future<String> ringing() async {
        app.foreground = false;
        final id = await other.call('laptop');
        await eventually(() => notifier.rang.isNotEmpty);
        expect(notifier.stoppedRinging, isEmpty);
        return id;
      }

      test('answered, the ring stops', () async {
        final id = await ringing();
        await app.talk.answer();
        await eventually(() => notifier.stoppedRinging.contains(id));
        app.talk.hangUp();
      });

      test('declined, the ring stops', () async {
        final id = await ringing();
        app.talk.decline();
        await eventually(() => notifier.stoppedRinging.contains(id));
      });

      test('ended by the caller, the ring stops', () async {
        final id = await ringing();
        other.endCall(id);
        await eventually(() => notifier.stoppedRinging.contains(id));
      });

      test('unanswered for 45 s, the ring stops', () async {
        final id = await ringing();
        app.peer.calls[id]!.since = DateTime.now().subtract(const Duration(seconds: 46));
        await eventually(() => notifier.stoppedRinging.contains(id));
        expect(app.talk.call?.state, 'ended:timeout');
        expect(notifier.stoppedRinging, [id], reason: 'stopped once');
      });
    });

    test('Android: whether calls may fill the screen is checked at start and when a call comes', () async {
      final perms = FakePermissions();
      await app.close();
      await other.stop();
      app = testModel(tailnet, notifier, audio: audio, permissions: perms);
      other = await otherPeer();
      await app.start();
      await eventually(() => perms.fillScreenChecks == 1);
      await befriend();
      perms.fillScreen = false;
      await other.call('laptop');
      await eventually(() => perms.fillScreenChecks == 2);
      expect(app.permissions!.callsFillScreen, isFalse);
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

    group('between two companion apps, on WebRTC', () {
      late ChatModel friend;
      late FakeCallEngine mine, theirs;
      late FakeAudio friendAudio;
      late RecordingNotifier friendNotifier;
      late String friendBox;

      setUp(() async {
        await app.close();
        await other.stop();
        mine = FakeCallEngine();
        theirs = FakeCallEngine();
        friendAudio = FakeAudio();
        app = testModel(tailnet, notifier, audio: audio, callEngine: mine);
        friendNotifier = RecordingNotifier();
        friend = testModel(tailnet, friendNotifier, audio: friendAudio, callEngine: theirs);
        other = await otherPeer(); // the T-Deck, for the ws-opus case
        await app.start();
        await friend.start();
        await befriend(); // with the T-Deck
        friend.peer.addContact(app.listening!, name: 'desk');
        friend.peer.sendText('desk', 'hi');
        await eventually(() => app.requests.isNotEmpty);
        app.accept(friend.identity.keyHex);
        await eventually(() => friend.peer.contact('desk')!.acceptedByThem == true);
        friendBox = friend.identity.keyHex;
      });

      tearDown(() => friend.close());

      Future<String> connected() async {
        final id = (await app.talk.startCall(friendBox))!;
        await eventually(() => friend.talk.call?.state == 'ringing');
        await friend.talk.answer();
        await eventually(() => app.talk.call?.state == 'connected' && friend.talk.call?.state == 'connected');
        return id;
      }

      test('the call is WebRTC, voice both ways, with WebRTC\'s echo canceller and no PipeWire one', () async {
        final id = await connected();
        expect(app.peer.calls[id]!.transport, 'webrtc');
        expect(friend.peer.calls[id]!.transports, ['webrtc', 'ws-opus']);
        await eventually(() => mine.sessions.isNotEmpty && theirs.sessions.isNotEmpty);
        expect(mine.session.offerer, isTrue);
        expect(theirs.session.offerer, isFalse);
        // The offer and the answer crossed, and the tailnet's candidate alone.
        await eventually(() => theirs.session.sdps.isNotEmpty && mine.session.sdps.isNotEmpty);
        expect(theirs.session.sdps.single['type'], 'offer');
        expect(mine.session.sdps.single['type'], 'answer');
        await eventually(() => mine.session.candidates.isNotEmpty && theirs.session.candidates.isNotEmpty);
        await Future<void>.delayed(const Duration(milliseconds: 200));
        expect(theirs.session.candidates, [FakeCallEngine.tailnetCandidate]);
        expect(mine.session.candidates, [FakeCallEngine.tailnetCandidate]);
        await eventually(() => app.peer.calls[id]!.media == 'connected' && friend.peer.calls[id]!.media == 'connected');
        expect(app.talk.echoCancellation, 'on (WebRTC, fake)');
        expect(audio.callsOpened, 0, reason: 'no ws-opus devices, no PipeWire canceller');
        expect(friendAudio.callsOpened, 0);
        // Each side says its camera is off, at the answer.
        await eventually(
          () => app.peer.calls[id]!.remoteCamera == 'off' && friend.peer.calls[id]!.remoteCamera == 'off',
        );
      });

      test('mute reaches the engine; a hangup ends the call and closes both sides\' media', () async {
        final id = await connected();
        await eventually(() => mine.sessions.isNotEmpty && theirs.sessions.isNotEmpty);
        app.talk.muted = true;
        expect(mine.session.muted, isTrue);
        app.talk.volume = 1.5;
        expect(mine.session.volume, 1.5);
        app.talk.hangUp();
        await eventually(() => friend.peer.calls[id]!.state == 'ended:hangup');
        await eventually(() => mine.session.closed && theirs.session.closed);
      });

      test('a call with the T-Deck, which has no call:webrtc, is ws-opus as before', () async {
        final id = (await app.talk.startCall(box))!;
        await eventually(() => app.talk.call?.state == 'connected');
        expect(app.peer.calls[id]!.transport, 'ws-opus');
        await eventually(() => audio.callsOpened == 1);
        expect(mine.sessions, isEmpty);
        app.talk.hangUp();
      });

      test('a connection whose selected pair leaves the tailnet ends the call as an error', () async {
        mine.path = const CallPath(
          local: '192.168.2.131',
          remote: '100.64.0.2',
          localType: 'prflx',
          remoteType: 'host',
        );
        final id = (await app.talk.startCall(friendBox))!;
        await eventually(() => friend.talk.call?.state == 'ringing');
        await friend.talk.answer();
        await eventually(() => app.peer.calls[id]!.state == 'ended:error');
        await eventually(() => friend.peer.calls[id]!.state == 'ended:error');
        expect(callOutcome('ended:error', transport: 'webrtc'), 'Call ended: the connection dropped');
      });

      test('media that never connects ends the call as not connected, and the screen says so', () async {
        mine.connects = false;
        theirs.connects = false;
        final id = await connected();
        app.peer.calls[id]!.since = DateTime.now().subtract(const Duration(seconds: 16));
        await eventually(() => app.peer.calls[id]!.state == 'ended:timeout');
        await eventually(() => friend.peer.calls[id]!.state == 'ended:timeout');
        await eventually(() => mine.session.closed);
        expect(callOutcome('ended:timeout', transport: 'webrtc'), 'Call ended: it could not connect');
        expect(callOutcome('ended:timeout', transport: 'ws-opus'), 'Call ended: no sound for 10 s');
      });

      group('video', () {
        test('a call started with video rings with `video`, and the caller\'s camera is on from the answer', () async {
          final id = (await app.talk.startCall(friendBox, video: true))!;
          await eventually(() => friend.talk.call?.state == 'ringing');
          expect(friend.peer.calls[id]!.video, isTrue);
          await friend.talk.answer();
          await eventually(() => friend.peer.calls[id]!.remoteCamera == 'on');
          expect(mine.session.camera, 'Integrated Webcam');
          expect(app.talk.cameraOn, isTrue);
        });

        test('the caller\'s camera switched on while it rings is on from the answer', () async {
          final id = (await app.talk.startCall(friendBox))!;
          await eventually(() => friend.talk.call?.state == 'ringing');
          await app.talk.setCameraOn(true);
          expect(app.talk.cameraSwitchOn, isTrue);
          await friend.talk.answer();
          await eventually(() => friend.peer.calls[id]!.remoteCamera == 'on');
          expect(mine.session.camera, 'Integrated Webcam');
        });

        test('a plain Answer leaves the camera off; the other side is told off', () async {
          final id = (await app.talk.startCall(friendBox, video: true))!;
          await eventually(() => friend.talk.call?.state == 'ringing');
          await friend.talk.answer();
          await eventually(() => app.peer.calls[id]!.remoteCamera == 'off');
          expect(theirs.session.cameraLog, isEmpty, reason: 'the camera never opened');
          expect(friend.talk.cameraOn, isFalse);
        });

        test('Answer with video turns the camera on and tells the other side', () async {
          final id = (await app.talk.startCall(friendBox))!;
          await eventually(() => friend.talk.call?.state == 'ringing');
          expect(friend.peer.calls[id]!.video, isFalse);
          await friend.talk.answer(null, true);
          await eventually(() => app.peer.calls[id]!.remoteCamera == 'on');
          expect(theirs.session.camera, 'Integrated Webcam');
          expect(mine.session.cameraLog, isEmpty, reason: 'the caller started with it off');
        });

        test('the camera switch replaces the track and tells the other side, each time', () async {
          final id = await connected();
          await eventually(() => mine.sessions.isNotEmpty && app.talk.cameras != null);
          expect(app.talk.canSwitchCamera, isFalse, reason: 'one camera');
          await app.talk.setCameraOn(true);
          await eventually(() => friend.peer.calls[id]!.remoteCamera == 'on');
          expect(mine.session.camera, 'Integrated Webcam');
          await app.talk.setCameraOn(false);
          await eventually(() => friend.peer.calls[id]!.remoteCamera == 'off');
          expect(mine.session.cameraLog, ['Integrated Webcam', null]);
          expect(mine.sessions, hasLength(1), reason: 'the same session: no renegotiation');
        });

        test('Switch camera goes through the cameras, two or more, round and back', () async {
          mine.cameraList = ['Integrated Webcam', 'USB Camera'];
          await connected();
          await eventually(() => app.talk.cameras != null);
          expect(app.talk.canSwitchCamera, isTrue);
          await app.talk.setCameraOn(true);
          await app.talk.switchCamera();
          expect(mine.session.camera, 'USB Camera');
          await app.talk.switchCamera();
          expect(mine.session.camera, 'Integrated Webcam');
        });

        test('no camera: it says none and still has the other side\'s video', () async {
          theirs.cameraList = [];
          final id = (await app.talk.startCall(friendBox, video: true))!;
          await eventually(() => friend.talk.call?.state == 'ringing');
          await friend.talk.answer(null, true);
          await eventually(() => app.peer.calls[id]!.remoteCamera == 'none');
          await eventually(() => friend.peer.calls[id]!.remoteCamera == 'on');
          expect(friend.talk.cameraOn, isFalse);
          expect(theirs.session.cameraLog, isEmpty);
          expect(friend.talk.remoteVideoShown, isTrue);
        });

        test('a camera that gives no picture is turned off, the other side told, and the screen says why', () async {
          final id = await connected();
          mine.picture = false;
          await eventually(() => app.talk.cameras != null);
          await app.talk.setCameraOn(true);
          expect(app.talk.cameraOn, isFalse);
          expect(app.talk.cameraProblem, contains('no picture'));
          await Future<void>.delayed(const Duration(milliseconds: 200));
          expect(friend.peer.calls[id]!.remoteCamera, 'off');
        });

        test('a video call rings as one, and unanswered is a missed video call', () async {
          friend.foreground = false;
          final id = (await app.talk.startCall(friendBox, video: true))!;
          await eventually(() => friendNotifier.rang.isNotEmpty);
          expect(friendNotifier.rangVideo.single, isTrue);
          app.talk.hangUp();
          await eventually(() => friendNotifier.shown.any((s) => s.title == 'Missed video call'));
          expect(friend.peer.calls[id]!.missed, isTrue);
        });

        test('while a camera is on, the app starts no child process: Tailscale\'s status is not read', () async {
          final id = await connected();
          await eventually(() => app.talk.cameras != null);
          await app.talk.setCameraOn(true);
          await eventually(() => friend.peer.calls[id]!.remoteCamera == 'on');
          final reads = tailnet.reads;
          await app.refresh();
          await app.peer.tailnetPeers!(); // a contact asks for the peer list
          expect(tailnet.reads, reads);
          await app.talk.setCameraOn(false);
          await app.refresh();
          expect(tailnet.reads, reads + 1);
        });
      });

      test('media lost for 10 s ends the call as an error', () async {
        final id = await connected();
        await eventually(() => app.peer.calls[id]!.media == 'connected');
        mine.session.state('disconnected');
        app.peer.calls[id]!.mediaSince = DateTime.now().subtract(const Duration(seconds: 11));
        await eventually(() => app.peer.calls[id]!.state == 'ended:error');
      });
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
