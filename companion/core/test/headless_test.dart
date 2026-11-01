import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:test/test.dart';

/// The headless entry point, driven over the control protocol as the
/// conformance suite drives it.
class Headless {
  Headless._(this.process, this.replies);

  final Process process;
  final StreamIterator<String> replies;
  int _seq = 0;

  static Future<(Headless, Map<String, dynamic>)> start(List<String> args) async {
    final p = await Process.start(Platform.resolvedExecutable, ['run', 'bin/tailnet_chat_core.dart', ...args]);
    p.stderr.drain<void>();
    final lines = StreamIterator(p.stdout.transform(utf8.decoder).transform(const LineSplitter()));
    if (!await lines.moveNext().timeout(const Duration(seconds: 60))) throw StateError('no ready line');
    final ready = jsonDecode(lines.current) as Map<String, dynamic>;
    return (Headless._(p, lines), ready['ready'] as Map<String, dynamic>);
  }

  Future<Object?> call(Map<String, Object?> command) async {
    final seq = ++_seq;
    process.stdin.writeln(jsonEncode({...command, 'seq': seq}));
    await process.stdin.flush();
    while (await replies.moveNext().timeout(const Duration(seconds: 10))) {
      final reply = jsonDecode(replies.current) as Map<String, dynamic>;
      if (reply['seq'] != seq) continue;
      if (reply.containsKey('error')) throw StateError(reply['error'] as String);
      return reply['ok'];
    }
    throw StateError('the process ended');
  }

  Future<int> stop() async {
    await call({'cmd': 'stop'});
    return process.exitCode.timeout(const Duration(seconds: 10));
  }
}

void main() {
  late Directory tmp;
  setUp(() async => tmp = await Directory.systemTemp.createTemp('tailnet_chat_headless'));
  tearDown(() => tmp.delete(recursive: true));

  test('an identity file and a database keep the key, the contacts and the outbox across restarts', () async {
    final args = [
      '--identity', '${tmp.path}/identity', //
      '--db', '${tmp.path}/chat.db',
      '--address', '127.0.0.1',
      '--port', '0',
      '--loopback',
    ];
    final (first, ready) = await Headless.start(args);
    expect(ready['key'], matches(RegExp(r'^[0-9a-f]{64}$')));
    expect((ready['address'] as List).first, '127.0.0.1');
    expect(File('${tmp.path}/identity').readAsStringSync().trim(), matches(RegExp(r'^[0-9a-f]{64}$')));
    if (!Platform.isWindows) {
      expect(File('${tmp.path}/identity').statSync().modeString(), 'rw-------');
    }
    await first.call({
      'cmd': 'add_contact',
      'address': ['127.0.0.1', 9],
      'name': 'nowhere',
    });
    final id = await first.call({'cmd': 'send_text', 'to': 'nowhere', 'text': 'later'});
    expect(await first.stop(), 0);

    final (second, again) = await Headless.start(args);
    expect(again['key'], ready['key']);
    final contacts = (await second.call({'cmd': 'contacts'})) as List;
    expect(contacts.map((c) => (c as Map)['name']), ['nowhere']);
    expect(await second.call({'cmd': 'waiting', 'to': 'nowhere'}), [id]);
    expect(await second.call({'cmd': 'messages', 'to': 'nowhere'}), [
      isA<Map>().having((m) => m['text'], 'text', 'later').having((m) => m['state'], 'state', 'waiting'),
    ]);
    expect(await second.stop(), 0);
  }, timeout: const Timeout(Duration(minutes: 2)));
}
