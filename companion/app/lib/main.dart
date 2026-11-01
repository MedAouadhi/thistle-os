import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import 'src/chat_model.dart';
import 'src/identity.dart';
import 'src/notifier.dart';
import 'src/options.dart';
import 'src/tailnet.dart';
import 'src/ui/app.dart';

Future<void> main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  final AppOptions options;
  try {
    options = AppOptions.parse(args);
  } on FormatException catch (e) {
    stderr.writeln(e.message);
    exit(64);
  }

  final Identity identity;
  try {
    final vault = SecretServiceVault(profile: options.profile);
    identity = await loadIdentity(vault);
    await vault.close();
  } on Object catch (e) {
    // The key is never kept in a plain file instead.
    runApp(
      _Failed(
        'The identity key could not be read from or kept in the Secret Service '
        '(GNOME Keyring or KWallet): $e',
      ),
    );
    return;
  }

  final dir = Directory('${(await getApplicationSupportDirectory()).path}/${options.profile}');
  await dir.create(recursive: true);
  final status = await readTailscaleStatus();
  final navigator = GlobalKey<NavigatorState>();
  late final ChatModel model;
  final notifier = SystemNotifier(
    onOpen: (box) {
      final nav = navigator.currentState;
      if (box != null && nav != null) openConversation(nav, model, box);
    },
  );
  await notifier.init();
  model = ChatModel(
    identity: identity,
    openStore: () => Store.open('${dir.path}/chat.db'),
    name: options.name ?? status.selfName ?? Platform.localHostname,
    notifier: notifier,
    listenHost: options.listenHost,
    port: options.port,
    loopback: options.loopback,
    discoveryPort: options.discoveryPort,
    log: options.verbose ? stderr.writeln : null,
  );
  await model.start();
  runApp(ThistleChatApp(model: model, navigatorKey: navigator));
}

class _Failed extends StatelessWidget {
  const _Failed(this.why);
  final String why;

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'Thistle Chat',
    home: Scaffold(
      body: Center(
        child: Padding(padding: const EdgeInsets.all(24), child: Text(why)),
      ),
    ),
  );
}
