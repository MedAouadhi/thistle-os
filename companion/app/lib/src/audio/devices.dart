/// Where the app's audio comes from and goes: the platform's microphone and
/// speaker (PulseAudio or PipeWire on Linux), with echo cancellation for a
/// call; or WAV files (`--mic`, `--speaker`), for a run without a sound
/// card and for tests.
library;

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import 'pulse.dart';

export 'pulse.dart' show AudioUnavailable;

/// A call's microphone and speaker, opened together.
class CallDevices {
  CallDevices(this.microphone, this.speaker, this.echoCancellation, this._close);
  final Stream<Int16List> microphone;
  final AudioOut speaker;

  /// What removes the speaker from the microphone, for the call screen.
  final String echoCancellation;
  final Future<void> Function() _close;

  /// Stop what was started for the call (the speaker is CallMedia's to close).
  Future<void> close() => _close();
}

abstract interface class AudioDevices {
  /// A microphone for a voice message.
  Future<Stream<Int16List>> recorder();

  /// A speaker for a voice message, or for the ring of a call coming in.
  Future<AudioOut> player({bool ring = false});

  /// A call's microphone and speaker, echo cancelled where the platform can.
  Future<CallDevices> call();
}

/// No audio on this platform (yet): Android until it bundles libopus and a
/// device layer.
class NoAudioDevices implements AudioDevices {
  NoAudioDevices(this.why);
  final String why;
  @override
  Future<Stream<Int16List>> recorder() => Future.error(AudioUnavailable(why));
  @override
  Future<AudioOut> player({bool ring = false}) => Future.error(AudioUnavailable(why));
  @override
  Future<CallDevices> call() => Future.error(AudioUnavailable(why));
}

/// PulseAudio's simple API, which PipeWire answers too.
///
/// Echo cancellation for a call, in order:
///  1. PipeWire: `pipewire -c <echo-cancel.conf>` for the call's length,
///     a client that loads libpipewire-module-echo-cancel (WebRTC's AEC)
///     and makes `thistle-chat-ec-source` and `thistle-chat-ec-sink`; the
///     call records from the one and plays to the other.
///  2. Otherwise the streams ask for it, `filter.want=echo-cancel` and
///     `media.role=phone`, which PulseAudio's module-filter-apply honours
///     (loaded by default).
class PulseAudioDevices implements AudioDevices {
  PulseAudioDevices({required this.configDir, this.log});

  /// Where the echo canceller's PipeWire configuration is written.
  final String configDir;
  final void Function(String)? log;

  static const ecSource = 'thistle-chat-ec-source';
  static const ecSink = 'thistle-chat-ec-sink';
  static const askForEchoCancel = 'media.role=phone filter.want=echo-cancel';

  @override
  Future<Stream<Int16List>> recorder() => pulseMicrophone(name: 'voice message');

  @override
  Future<AudioOut> player({bool ring = false}) => pulseSpeaker(name: ring ? 'ring' : 'voice message');

  @override
  Future<CallDevices> call() async {
    final ec = await _startPipewireCanceller();
    if (ec != null) {
      try {
        final mic = await ec.$2;
        final speaker = await pulseSpeaker(device: ecSink, name: 'call');
        return CallDevices(mic, speaker, 'on (PipeWire, WebRTC echo canceller)', () async => ec.$1.kill());
      } on AudioUnavailable catch (e) {
        log?.call('echo canceller: $e');
        ec.$1.kill();
      }
    }
    final mic = await pulseMicrophone(name: 'call', props: askForEchoCancel);
    final speaker = await pulseSpeaker(name: 'call', props: askForEchoCancel);
    return CallDevices(
      mic,
      speaker,
      'asked of the sound server (PulseAudio module-echo-cancel, if it applies it)',
      () async {},
    );
  }

  /// PipeWire's canceller and its microphone, once its nodes are up; null
  /// without PipeWire.
  Future<(Process, Future<Stream<Int16List>>)?> _startPipewireCanceller() async {
    final Process p;
    try {
      final conf = File('$configDir/echo-cancel.conf');
      await conf.parent.create(recursive: true);
      await conf.writeAsString(echoCancelConf);
      p = await Process.start('pipewire', ['-c', conf.path]);
    } on Object catch (e) {
      log?.call('no PipeWire echo canceller: $e');
      return null;
    }
    unawaited(p.stdout.drain<void>());
    unawaited(p.stderr.drain<void>());
    var exited = false;
    unawaited(p.exitCode.then((_) => exited = true));
    // The source appears a moment after the client starts.
    for (var i = 0; i < 20 && !exited; i++) {
      await Future<void>.delayed(const Duration(milliseconds: 100));
      try {
        final mic = await pulseMicrophone(device: ecSource, name: 'call');
        return (p, Future.value(mic));
      } on AudioUnavailable {
        continue;
      }
    }
    p.kill();
    log?.call('the PipeWire echo canceller did not come up');
    return null;
  }
}

/// WAV files instead of a sound card: the microphone plays `mic` over and
/// over (silence without it), and each speaker writes a numbered WAV file
/// into `speakerDir` when it closes (nothing without it).
class FileAudioDevices implements AudioDevices {
  FileAudioDevices({this.mic, this.speakerDir});
  final String? mic;
  final String? speakerDir;
  int _n = 0;

  Stream<Int16List> _mic() => mic == null ? pacedAudio((_) => Int16List(samplesPerFrame)) : wavFileIn(mic!);

  AudioOut _out(String what) {
    if (speakerDir == null) return NoAudioOut();
    Directory(speakerDir!).createSync(recursive: true);
    return WavFileOut('$speakerDir/${(++_n).toString().padLeft(3, '0')}-$what.wav');
  }

  @override
  Future<Stream<Int16List>> recorder() async => _mic();
  @override
  Future<AudioOut> player({bool ring = false}) async => _out(ring ? 'ring' : 'played');
  @override
  Future<CallDevices> call() async => CallDevices(_mic(), _out('call'), 'none (audio files)', () async {});
}

/// libpipewire-module-echo-cancel as a client of the running PipeWire;
/// nothing in the user's own configuration changes.
const echoCancelConf = '''
# Thistle Chat's echo canceller, written by the app and run as
# `pipewire -c <this file>` while a call is up.
context.properties = {
    log.level = 0
}
context.spa-libs = {
    audio.convert.* = audioconvert/libspa-audioconvert
    support.*       = support/libspa-support
}
context.modules = [
    { name = libpipewire-module-rt
        args = { }
        flags = [ ifexists nofail ]
    }
    { name = libpipewire-module-protocol-native }
    { name = libpipewire-module-client-node }
    { name = libpipewire-module-adapter }
    { name = libpipewire-module-echo-cancel
        args = {
            library.name = aec/libspa-aec-webrtc
            audio.rate = 16000
            audio.channels = 1
            audio.position = [ MONO ]
            capture.props = {
                node.name = "thistle-chat-ec-capture"
                node.passive = true
            }
            source.props = {
                node.name = "thistle-chat-ec-source"
                node.description = "Thistle Chat microphone (echo cancelled)"
            }
            sink.props = {
                node.name = "thistle-chat-ec-sink"
                node.description = "Thistle Chat speaker (echo reference)"
            }
            playback.props = {
                node.name = "thistle-chat-ec-playback"
                node.passive = true
            }
        }
    }
]
''';
