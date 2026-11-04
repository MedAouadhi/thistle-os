import 'dart:typed_data';

import 'package:flutter/services.dart';
import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/audio/android_audio.dart';
import 'package:thistle_chat/src/audio/devices.dart';

/// The Kotlin side of the audio channels (Audio.kt), as the Dart side sees
/// it: the calls it was sent, and the microphone's frames it sends.
class FakeAndroidAudio {
  FakeAndroidAudio() {
    final m = TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger;
    m.setMockMethodCallHandler(AndroidAudioDevices.control, (call) async {
      calls.add('${call.method} ${_args(call.arguments)}');
      final refuse = refusing[call.method];
      if (refuse != null) throw PlatformException(code: 'audio', message: refuse);
      return switch (call.method) {
        'mic.open' => echo,
        'speaker.open' => ++_speakers,
        _ => null,
      };
    });
    m.setMockStreamHandler(
      AndroidAudioDevices.microphone,
      MockStreamHandler.inline(
        onListen: (_, sink) => this.sink = sink,
        onCancel: (_) {
          calls.add('microphone cancelled');
          sink = null;
        },
      ),
    );
  }

  final calls = <String>[];
  final refusing = <String, String>{};
  String? echo = 'on (AcousticEchoCanceler)';
  MockStreamHandlerEventSink? sink;
  int _speakers = 0;

  static String _args(Object? a) {
    if (a is! Map) return '$a';
    return a.entries
        .map((e) => '${e.key}=${e.value is Uint8List ? (e.value as Uint8List).toList() : e.value}')
        .join(' ');
  }
}

Int16List _frame(int v) => Int16List(320)..fillRange(0, 320, v);

void main() {
  TestWidgetsFlutterBinding.ensureInitialized();

  test('a voice message records from the microphone, 20 ms frames, until cancelled', () async {
    final fake = FakeAndroidAudio();
    final mic = await AndroidAudioDevices().recorder();
    final got = <Int16List>[];
    final sub = mic.listen(got.add);
    await pumpEventQueue();
    fake.sink!.success(_frame(7).buffer.asUint8List());
    fake.sink!.success(_frame(-3).buffer.asUint8List());
    await pumpEventQueue();
    expect(got.map((f) => (f.length, f.first, f.last)), [(320, 7, 7), (320, -3, -3)]);
    await sub.cancel();
    expect(fake.calls, ['mic.open call=false', 'microphone cancelled', 'mic.close null']);
  });

  test('a microphone Android will not open is no audio, with its reason', () async {
    final fake = FakeAndroidAudio()..refusing['mic.open'] = 'AudioRecord did not initialise';
    await expectLater(
      AndroidAudioDevices().recorder(),
      throwsA(isA<AudioUnavailable>().having((e) => e.why, 'why', 'AudioRecord did not initialise')),
    );
    expect(fake.calls, ['mic.open call=false']);
  });

  test('a voice message plays to the media speaker, its PCM as little-endian bytes, drained at close', () async {
    final fake = FakeAndroidAudio();
    final out = await AndroidAudioDevices().player();
    out.write(Int16List.fromList([1, -2, 258]));
    await out.close();
    out.write(Int16List.fromList([5])); // after close: nothing
    await pumpEventQueue();
    expect(fake.calls, [
      'speaker.open use=voice',
      'speaker.write id=1 pcm=[1, 0, 254, 255, 2, 1]',
      'speaker.close id=1',
    ]);
  });

  test(
    'a call begins on the platform, records with its echo canceller and plays on the call stream, then ends',
    () async {
      final fake = FakeAndroidAudio();
      final devices = await AndroidAudioDevices().call();
      expect(devices.echoCancellation, 'on (AcousticEchoCanceler)');
      final sub = devices.microphone.listen((_) {});
      await pumpEventQueue();
      await sub.cancel();
      await devices.speaker.close();
      await devices.close();
      expect(fake.calls, [
        'call.begin null',
        'mic.open call=true',
        'speaker.open use=call',
        'microphone cancelled',
        'mic.close null',
        'speaker.close id=1',
        'call.end null',
      ]);
    },
  );

  test('a phone with no echo canceller says so on the call screen', () async {
    final fake = FakeAndroidAudio()..echo = null;
    final devices = await AndroidAudioDevices().call();
    expect(devices.echoCancellation, 'none (this phone has no AcousticEchoCanceler)');
    expect(fake.calls.first, 'call.begin null');
  });

  test('a call whose microphone will not open ends on the platform and is no audio', () async {
    final fake = FakeAndroidAudio()..refusing['mic.open'] = 'no microphone';
    await expectLater(AndroidAudioDevices().call(), throwsA(isA<AudioUnavailable>()));
    expect(fake.calls, ['call.begin null', 'mic.open call=true', 'call.end null']);
  });

  test('a call whose speaker will not open lets its microphone go too', () async {
    final fake = FakeAndroidAudio()..refusing['speaker.open'] = 'no AudioTrack';
    await expectLater(AndroidAudioDevices().call(), throwsA(isA<AudioUnavailable>()));
    expect(fake.calls, [
      'call.begin null',
      'mic.open call=true',
      'speaker.open use=call',
      'mic.close null',
      'call.end null',
    ]);
  });

  test('a ring plays as a ringtone', () async {
    final fake = FakeAndroidAudio();
    await (await AndroidAudioDevices().player(ring: true)).close();
    expect(fake.calls, ['speaker.open use=ring', 'speaker.close id=1']);
  });

  group("a WebRTC call's route", () {
    /// flutter_webrtc's own channel: its audio switch picks the output.
    late List<String> webrtc;
    setUp(() {
      webrtc = [];
      TestDefaultBinaryMessengerBinding.instance.defaultBinaryMessenger.setMockMethodCallHandler(
        const MethodChannel('FlutterWebRTC.Method'),
        (call) async {
          if (call.method != 'initialize') webrtc.add('${call.method} ${call.arguments}');
          return null;
        },
      );
    });

    test('begins on the earpiece with the screen dark at the ear; video moves it to the loudspeaker, a headset '
        'first; the end undoes it', () async {
      final fake = FakeAndroidAudio();
      final route = AndroidCallRoute();
      await route.begin();
      await route.video(true);
      await route.video(false);
      await route.end();
      expect(fake.calls, ['rtc.begin null', 'rtc.video shown=true', 'rtc.video shown=false', 'rtc.end null']);
      expect(webrtc, [
        'enableSpeakerphone {enable: false}',
        'enableSpeakerphoneButPreferBluetooth null',
        'enableSpeakerphone {enable: false}',
      ]);
    });

    test('a refusal of either side is logged, not thrown: the call goes on', () async {
      final fake = FakeAndroidAudio()..refusing['rtc.video'] = 'no window';
      final said = <String>[];
      await AndroidCallRoute(log: said.add).video(true);
      expect(fake.calls, ['rtc.video shown=true']);
      expect(said.single, contains('no window'));
    });
  });
}
