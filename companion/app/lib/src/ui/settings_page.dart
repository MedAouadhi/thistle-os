import 'package:flutter/material.dart';

import '../chat_model.dart';
import 'app.dart' show PageTitle;
import 'theme.dart';
import 'widgets.dart';

class SettingsPage extends StatelessWidget {
  const SettingsPage({super.key, required this.model});
  final ChatModel model;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    final listening = model.listening;
    final phone = MediaQuery.sizeOf(context).width < 600;
    return ListView(
      padding: const EdgeInsets.fromLTRB(12, 0, 12, 20),
      children: [
        if (phone) const PageTitle('Settings'),
        const SectionLabel('This app'),
        Card(
          child: Padding(
            padding: const EdgeInsets.all(18),
            child: Row(
              crossAxisAlignment: CrossAxisAlignment.start,
              children: [
                Monogram(name: model.name, keyHex: model.identity.keyHex, size: 52, online: listening != null),
                const SizedBox(width: 14),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text('This app: ${model.name}', style: display(19, color: k.ink)),
                      const SizedBox(height: 8),
                      SelectableText(
                        'Fingerprint ${model.identity.fingerprint}\n'
                        '${listening == null ? 'Not listening' : 'Listening on $listening'}',
                        style: mono(12.5, color: k.muted).copyWith(height: 1.6),
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
        const SectionLabel('Privacy and reach'),
        Card(
          clipBehavior: Clip.antiAlias,
          child: Column(
            children: [
              SwitchListTile(
                title: const Text('Send read receipts'),
                subtitle: const Text('Contacts see when you have read their messages.'),
                value: model.readReceipts,
                onChanged: (on) => model.readReceipts = on,
              ),
              Divider(indent: 18, endIndent: 18, color: k.line),
              SwitchListTile(
                title: const Text('Listen in the background'),
                subtitle: Text(
                  model.background != null
                      ? 'A persistent notification keeps the app reachable after it is swiped away. '
                            'Off, messages wait with their sender until you open the app.'
                      : 'On Android, a persistent notification keeps the app reachable. '
                            'On Linux the app listens while it runs.',
                ),
                value: model.backgroundListening,
                onChanged: (on) => model.backgroundListening = on,
              ),
            ],
          ),
        ),
        const SizedBox(height: 28),
        Center(
          child: Text(
            'Peer to peer over your tailnet. No server in between.',
            style: mono(11, color: k.muted),
            textAlign: TextAlign.center,
          ),
        ),
      ],
    );
  }
}
