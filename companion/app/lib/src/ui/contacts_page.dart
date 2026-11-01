import 'package:flutter/material.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import '../chat_model.dart';
import 'conversation_page.dart';

String fingerprintOfHex(String key) => fingerprintOf(fromHex(key));

/// Discovery (the chat peers among the online tailnet peers), a peer by
/// name or address, and the contacts.
class ContactsPage extends StatelessWidget {
  const ContactsPage({super.key, required this.model});
  final ChatModel model;

  Future<void> _addByName(BuildContext context) async {
    final added = await showDialog<Contact>(
      context: context,
      builder: (_) => _AddByNameDialog(model: model),
    );
    if (added != null && context.mounted) {
      ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text('Added ${added.name}')));
    }
  }

  @override
  Widget build(BuildContext context) {
    final contacts = model.contacts;
    final discovered = model.discovered;
    final small = Theme.of(context).textTheme.bodySmall;
    return ListView(
      children: [
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 8, 0),
          child: Row(
            children: [
              Expanded(child: Text('On your tailnet', style: Theme.of(context).textTheme.titleSmall)),
              if (model.discovering)
                const Padding(
                  padding: EdgeInsets.all(12),
                  child: SizedBox(width: 20, height: 20, child: CircularProgressIndicator(strokeWidth: 2)),
                )
              else
                IconButton(
                  tooltip: 'Look for chat peers',
                  icon: const Icon(Icons.search),
                  onPressed: model.status.state.name == 'running' ? model.discover : null,
                ),
              IconButton(
                tooltip: 'Add by name or address',
                icon: const Icon(Icons.person_add_alt),
                onPressed: () => _addByName(context),
              ),
            ],
          ),
        ),
        if (discovered.isEmpty)
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 4),
            child: Text(
              'Look for chat peers among the machines Tailscale shows online, or add one by name or address.',
              style: small,
            ),
          ),
        for (final d in discovered)
          ListTile(
            leading: const Icon(Icons.devices),
            title: Text(d.probe.name),
            subtitle: Text('${d.peer.hostName}, ${d.probe.address}\n${d.probe.fingerprint}'),
            isThreeLine: true,
            trailing: TextButton(onPressed: () => model.addDiscovered(d), child: const Text('Add')),
          ),
        const Divider(),
        Padding(
          padding: const EdgeInsets.fromLTRB(16, 8, 16, 0),
          child: Text('Contacts', style: Theme.of(context).textTheme.titleSmall),
        ),
        if (contacts.isEmpty)
          Padding(
            padding: const EdgeInsets.all(16),
            child: Text('No contacts yet.', style: small),
          ),
        for (final c in contacts)
          ListTile(
            leading: Icon(c.flaggedKey != null ? Icons.gpp_maybe : Icons.person),
            iconColor: c.flaggedKey != null ? Theme.of(context).colorScheme.error : null,
            title: Text(c.name),
            subtitle: Text(
              c.flaggedKey != null
                  ? 'Key changed: check it before you accept it'
                  : c.key == null
                  ? 'Key not known yet: pinned at the first connection'
                  : fingerprintOfHex(c.key!),
            ),
            onTap: () => Navigator.of(context).push(
              MaterialPageRoute<void>(
                builder: (_) => ContactPage(model: model, contactId: c.id),
              ),
            ),
          ),
      ],
    );
  }
}

class _AddByNameDialog extends StatefulWidget {
  const _AddByNameDialog({required this.model});
  final ChatModel model;

  @override
  State<_AddByNameDialog> createState() => _AddByNameDialogState();
}

class _AddByNameDialogState extends State<_AddByNameDialog> {
  final _text = TextEditingController();
  String? _error;
  bool _busy = false;

  Future<void> _add() async {
    setState(() {
      _busy = true;
      _error = null;
    });
    try {
      final c = await widget.model.addByName(_text.text);
      if (mounted) Navigator.pop(context, c);
    } on ArgumentError catch (e) {
      setState(() => _error = '${e.message}');
    } finally {
      if (mounted) setState(() => _busy = false);
    }
  }

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => AlertDialog(
    title: const Text('Add a chat peer'),
    content: TextField(
      key: const Key('add-by-name'),
      controller: _text,
      autofocus: true,
      decoration: InputDecoration(
        labelText: 'Tailnet name or address',
        hintText: 'pixel-8, 100.101.102.103, or host:4810',
        errorText: _error,
      ),
      onSubmitted: (_) => _add(),
    ),
    actions: [
      TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      FilledButton(onPressed: _busy ? null : _add, child: const Text('Add')),
    ],
  );
}

/// One contact: its key's fingerprint to compare, a changed key to accept,
/// rename, remove.
class ContactPage extends StatelessWidget {
  const ContactPage({super.key, required this.model, required this.contactId});
  final ChatModel model;
  final int contactId;

  Future<void> _rename(BuildContext context, Contact c) async {
    final name = await showDialog<String>(context: context, builder: (_) => _RenameDialog(c.name));
    if (name != null && name.trim().isNotEmpty) model.rename(c.box, name.trim());
  }

  Future<void> _remove(BuildContext context, Contact c) async {
    final sure = await showDialog<bool>(
      context: context,
      builder: (context) => AlertDialog(
        title: Text('Remove ${c.name}?'),
        content: const Text(
          'Its key is forgotten. If it writes again, it is a contact request again. '
          'The conversation is kept until then.',
        ),
        actions: [
          TextButton(onPressed: () => Navigator.pop(context, false), child: const Text('Cancel')),
          FilledButton(onPressed: () => Navigator.pop(context, true), child: const Text('Remove')),
        ],
      ),
    );
    if (sure == true && context.mounted) {
      model.remove(c.box);
      Navigator.pop(context);
    }
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: model,
    builder: (context, _) {
      final c = model.contacts.where((c) => c.id == contactId).firstOrNull;
      if (c == null) return const Scaffold(body: Center(child: Text('This contact was removed.')));
      final scheme = Theme.of(context).colorScheme;
      return Scaffold(
        appBar: AppBar(title: Text(c.name)),
        body: ListView(
          padding: const EdgeInsets.all(8),
          children: [
            if (c.flaggedKey != null)
              Card(
                color: scheme.errorContainer,
                child: Padding(
                  padding: const EdgeInsets.all(16),
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Text(
                        'Key changed',
                        style: Theme.of(context).textTheme.titleMedium?.copyWith(color: scheme.onErrorContainer),
                      ),
                      const SizedBox(height: 8),
                      Text(
                        'A chat peer using ${c.name}\'s name or address proved a different key, and was refused. '
                        'That happens when an app is reinstalled or a device loses its identity; it is also what '
                        'an impostor on your tailnet would look like. Compare the new fingerprint with the one on '
                        'its screen before you accept it.\n\n'
                        'New: ${fingerprintOfHex(c.flaggedKey!)}',
                        style: TextStyle(color: scheme.onErrorContainer),
                      ),
                      const SizedBox(height: 12),
                      FilledButton(onPressed: () => model.acceptNewKey(c.box), child: const Text('Accept the new key')),
                    ],
                  ),
                ),
              ),
            ListTile(
              leading: const Icon(Icons.fingerprint),
              title: const Text('Fingerprint'),
              subtitle: SelectableText(c.key == null ? 'Not known yet' : fingerprintOfHex(c.key!)),
            ),
            if (c.address != null)
              ListTile(leading: const Icon(Icons.lan), title: const Text('Address'), subtitle: Text('${c.address}')),
            if (c.tailnetName != null)
              ListTile(
                leading: const Icon(Icons.dns),
                title: const Text('Tailnet name'),
                subtitle: Text(c.tailnetName!),
              ),
            ListTile(
              leading: const Icon(Icons.how_to_reg),
              title: const Text('Our request'),
              subtitle: Text(switch ((c.acceptedByThem, c.declinedByThem)) {
                (_, true) => 'Declined: nothing more is sent',
                (true, _) => 'Accepted',
                _ => 'Not answered yet (or they wrote first)',
              }),
            ),
            const SizedBox(height: 8),
            Wrap(
              spacing: 8,
              runSpacing: 8,
              children: [
                FilledButton.icon(
                  icon: const Icon(Icons.chat),
                  label: const Text('Message'),
                  onPressed: () => Navigator.of(context).push(
                    MaterialPageRoute<void>(
                      builder: (_) => ConversationPage(model: model, contactId: c.id),
                    ),
                  ),
                ),
                OutlinedButton.icon(
                  icon: const Icon(Icons.edit),
                  label: const Text('Rename'),
                  onPressed: () => _rename(context, c),
                ),
                OutlinedButton.icon(
                  icon: const Icon(Icons.person_remove),
                  label: const Text('Remove'),
                  onPressed: () => _remove(context, c),
                ),
              ],
            ),
          ],
        ),
      );
    },
  );
}

class _RenameDialog extends StatefulWidget {
  const _RenameDialog(this.name);
  final String name;

  @override
  State<_RenameDialog> createState() => _RenameDialogState();
}

class _RenameDialogState extends State<_RenameDialog> {
  late final _text = TextEditingController(text: widget.name);

  @override
  void dispose() {
    _text.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => AlertDialog(
    title: const Text('Rename'),
    content: TextField(
      key: const Key('rename'),
      controller: _text,
      autofocus: true,
      onSubmitted: (v) => Navigator.pop(context, v),
    ),
    actions: [
      TextButton(onPressed: () => Navigator.pop(context), child: const Text('Cancel')),
      FilledButton(onPressed: () => Navigator.pop(context, _text.text), child: const Text('Save')),
    ],
  );
}
