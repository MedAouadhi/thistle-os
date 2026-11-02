import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/options.dart';

void main() {
  test('by default: the tailnet address, port 4810, the default profile', () {
    final o = AppOptions.parse(const []);
    expect(o.profile, 'default');
    expect(o.listenHost, isNull);
    expect(o.port, 4810);
    expect(o.loopback, isFalse);
    expect(o.name, isNull);
    expect(o.verbose, isFalse);
  });

  test('a second instance on one machine: its own profile and port', () {
    final o = AppOptions.parse(const ['--profile', 'second', '--port', '4811', '--name', 'laptop-2', '--verbose']);
    expect(o.profile, 'second');
    expect(o.port, 4811);
    expect(o.name, 'laptop-2');
    expect(o.verbose, isTrue);
  });

  test('--listen and --loopback, for a chat with a peer on this machine', () {
    final o = AppOptions.parse(const ['--listen', '127.0.0.1', '--loopback', '--discovery-port', '4820']);
    expect(o.listenHost, '127.0.0.1');
    expect(o.loopback, isTrue);
    expect(o.discoveryPort, 4820);
  });

  test('--mic and --speaker: audio files instead of a sound card', () {
    final o = AppOptions.parse(const ['--mic', 'tone.wav', '--speaker', '/tmp/heard']);
    expect(o.mic, 'tone.wav');
    expect(o.speakerDir, '/tmp/heard');
    expect(AppOptions.parse(const []).mic, isNull);
  });

  test('a bad option says so', () {
    expect(() => AppOptions.parse(const ['--port', 'x']), throwsFormatException);
    expect(() => AppOptions.parse(const ['--what']), throwsFormatException);
    expect(() => AppOptions.parse(const ['--profile', '../etc']), throwsFormatException);
  });
}
