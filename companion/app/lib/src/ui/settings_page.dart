import 'package:flutter/material.dart';

import '../chat_model.dart';

class SettingsPage extends StatelessWidget {
  const SettingsPage({super.key, required this.model});
  final ChatModel model;

  @override
  Widget build(BuildContext context) {
    final listening = model.listening;
    return ListView(
      children: [
        ListTile(
          leading: const Icon(Icons.fingerprint),
          title: Text('This app: ${model.name}'),
          subtitle: SelectableText(
            'Fingerprint ${model.identity.fingerprint}\n'
            '${listening == null ? 'Not listening' : 'Listening on $listening'}',
          ),
          isThreeLine: true,
        ),
        const Divider(),
        SwitchListTile(
          title: const Text('Send read receipts'),
          subtitle: const Text('Contacts see when you have read their messages.'),
          value: model.readReceipts,
          onChanged: (on) => model.readReceipts = on,
        ),
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
    );
  }
}
