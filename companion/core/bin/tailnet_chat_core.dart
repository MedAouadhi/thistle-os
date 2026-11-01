/// The companion core run headless: a chat peer driven by the control
/// protocol on stdin and stdout (docs/tailnet-chat/README.md), as the
/// conformance suite drives the reference chat peer.
///
///     dart run bin/tailnet_chat_core.dart --identity ~/.tailnet-chat/id \
///         --db ~/.tailnet-chat/chat.db --address 100.101.102.103 --port 4810
///
/// Logs go to stderr (with --verbose), never stdout.
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';

const usage = '''
usage: tailnet_chat_core [options]
  --identity FILE     the identity key's seed, hex; made if missing (default: a key for this run only)
  --db PATH           the database (default :memory:, nothing kept)
  --address HOST      the address to listen on, the tailnet address (default 127.0.0.1)
  --port N            the port (default 4810; 0 picks one)
  --listen HOST:PORT  --address and --port at once
  --name NAME         the name shown in hello (default: the host name)
  --loopback          admit loopback too, for tests
  --no-read-receipts  send no read receipts
  --call-policy P     answer, decline, busy or ignore (default answer)
  --verbose           log to stderr''';

Never fail(String why) {
  stderr.writeln('$why\n$usage');
  exit(2);
}

Future<void> main(List<String> args) async {
  String? identityPath, db = ':memory:', host = '127.0.0.1', name, callPolicy;
  var port = defaultPort;
  var loopback = false, verbose = false, readReceipts = true;
  for (var i = 0; i < args.length; i++) {
    String value() => i + 1 < args.length ? args[++i] : fail('${args[i]} needs a value');
    switch (args[i]) {
      case '--identity':
        identityPath = value();
      case '--db':
        db = value();
      case '--address':
        host = value();
      case '--port':
        port = int.tryParse(value()) ?? fail('--port takes a number');
      case '--listen':
        final v = value();
        final at = v.lastIndexOf(':');
        if (at < 0) fail('--listen takes HOST:PORT');
        host = v.substring(0, at).replaceAll('[', '').replaceAll(']', '');
        port = int.tryParse(v.substring(at + 1)) ?? fail('--listen takes HOST:PORT');
      case '--name':
        name = value();
      case '--loopback':
        loopback = true;
      case '--no-read-receipts':
        readReceipts = false;
      case '--call-policy':
        callPolicy = value();
      case '--verbose':
        verbose = true;
      case '-h' || '--help':
        stdout.writeln(usage);
        return;
      default:
        fail('unknown option ${args[i]}');
    }
  }

  final peer = ChatPeer(
    identity: identityPath == null ? Identity.generate() : loadOrCreateIdentity(identityPath),
    store: Store.open(db!),
    name: name ?? Platform.localHostname,
    host: host!,
    port: port,
    loopback: loopback,
    callPolicy: callPolicy == null ? CallPolicy.answer : CallPolicy.values.byName(callPolicy),
    log: verbose ? stderr.writeln : null,
  );
  if (!readReceipts) peer.readReceipts = false;
  await peer.start();
  stdout.writeln(jsonEncode({'ready': identityJson(peer)}));

  // One command at a time, in order, each answered before the next is read.
  final lines = stdin.transform(utf8.decoder).transform(const LineSplitter());
  await for (final line in lines) {
    if (line.trim().isEmpty) continue;
    Object? command;
    Map<String, Object?> reply;
    try {
      command = jsonDecode(line);
      if (command is! Map<String, dynamic>) throw const FormatException('a command is a JSON object');
      reply = {'ok': await control(peer, command)};
    } on Object catch (e) {
      reply = {'error': '${e.runtimeType}: $e'};
    }
    if (command is Map && command.containsKey('seq')) reply['seq'] = command['seq'];
    stdout.writeln(jsonEncode(reply));
    if (command is Map && command['cmd'] == 'stop') break;
  }
  await peer.stop();
  await stdout.flush();
  exit(0);
}
