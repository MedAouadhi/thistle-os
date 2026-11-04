import 'dart:async';
import 'dart:io';

import 'package:flutter/material.dart';
import 'package:path_provider/path_provider.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import 'src/android.dart';
import 'src/audio/android_audio.dart';
import 'src/audio/devices.dart';
import 'src/chat_model.dart';
import 'src/identity.dart';
import 'src/notifier.dart';
import 'src/options.dart';
import 'src/permissions.dart';
import 'src/tailnet.dart';
import 'src/webrtc_engine.dart';
import 'src/ui/app.dart';
import 'src/ui/call_page.dart';

Future<void> main(List<String> args) async {
  WidgetsFlutterBinding.ensureInitialized();
  final AppOptions options;
  try {
    options = AppOptions.parse(args);
  } on FormatException catch (e) {
    stderr.writeln(e.message);
    exit(64);
  }

  // Android: the Keystore, the listening service and the permissions over
  // the app's own channel; Linux: the Secret Service, and none of the rest.
  final android = Platform.isAndroid ? AndroidPlatform(profile: options.profile) : null;

  final Identity identity;
  try {
    if (android != null) {
      identity = await loadIdentity(android);
    } else {
      final vault = SecretServiceVault(profile: options.profile);
      identity = await loadIdentity(vault);
      await vault.close();
    }
  } on Object catch (e) {
    // The key is never kept in a plain file instead.
    runApp(
      _Failed(
        android != null
            ? 'The identity key could not be read from or kept in the Android Keystore: $e'
            : 'The identity key could not be read from or kept in the Secret Service '
                  '(GNOME Keyring or KWallet): $e',
      ),
    );
    return;
  }

  final dir = Directory('${(await getApplicationSupportDirectory()).path}/${options.profile}');
  await dir.create(recursive: true);
  final readStatus = android != null ? readAndroidTailnet : readTailscaleStatus;
  final status = await readStatus();
  final navigator = GlobalKey<NavigatorState>();
  late final ChatModel model;
  // On Android the phone rings for a call, with a call window over the
  // lock screen; the notification's Answer and Decline come back here.
  android?.onCall = (id, answer) => answer ? unawaited(model.talk.answer(id)) : model.talk.decline(id);
  final notifier = SystemNotifier(
    ringer: android,
    onOpen: (box) {
      final nav = navigator.currentState;
      if (box != null && nav != null) openConversation(nav, model, box);
    },
    onCall: (id, answer) {
      if (answer == true) unawaited(model.talk.answer(id));
      if (answer == false) model.talk.decline(id);
      final ctx = navigator.currentContext;
      if (answer != false && ctx != null) showCall(ctx, model);
    },
  );
  // The microphone and speaker: audio files when asked (no sound card, a
  // second instance, a test run), else AudioRecord/AudioTrack on Android
  // and PulseAudio/PipeWire on Linux.
  final AudioDevices audio = options.mic != null || options.speakerDir != null
      ? FileAudioDevices(mic: options.mic, speakerDir: options.speakerDir)
      : android != null
      ? AndroidAudioDevices()
      : PulseAudioDevices(configDir: dir.path, log: options.verbose ? stderr.writeln : null);
  await notifier.init();
  model = ChatModel(
    identity: identity,
    openStore: () => Store.open('${dir.path}/chat.db'),
    name: options.name ?? status.selfName ?? await android?.deviceName() ?? Platform.localHostname,
    readStatus: readStatus,
    background: android,
    permissions: android == null ? null : Permissions(android),
    audio: audio,
    // Calls with another companion app on WebRTC, on Linux for now; Android
    // announces no call:webrtc until ticket 05 of .scratch/tailnet-video.
    callEngine: Platform.isLinux && options.webrtc
        ? WebrtcCallEngine(log: options.verbose ? stderr.writeln : null)
        : null,
    notifier: notifier,
    listenHost: options.listenHost,
    port: options.port,
    loopback: options.loopback,
    discoveryPort: options.discoveryPort,
    log: options.verbose ? stderr.writeln : null,
  );
  await model.start();
  if (android != null) {
    // Started by the listening service after Android restarted it, there
    // is no window: messages notify until one shows.
    model.foreground = WidgetsBinding.instance.lifecycleState == AppLifecycleState.resumed;
  }
  runApp(ThistleChatApp(model: model, navigatorKey: navigator, callWindow: android?.callWindow));
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
