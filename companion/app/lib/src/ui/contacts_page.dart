import 'dart:math' as math;

import 'package:flutter/material.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import '../chat_model.dart';
import 'app.dart' show PageTitle;
import 'conversation_page.dart';
import 'theme.dart';
import 'widgets.dart';

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
    final k = Palette.of(context);
    final contacts = model.contacts;
    final discovered = model.discovered;
    final small = Theme.of(context).textTheme.bodySmall;
    final phone = MediaQuery.sizeOf(context).width < 600;
    final actions = Row(
      mainAxisSize: MainAxisSize.min,
      children: [
        // Android has no peer list of its own: it looks through those the contacts share.
        _LookButton(
          looking: model.discovering,
          onPressed: model.status.state.name == 'running' ? model.discover : null,
        ),
        const SizedBox(width: 4),
        IconButton(
          tooltip: 'Add by name or address',
          icon: const Icon(Icons.person_add_alt_rounded),
          onPressed: () => _addByName(context),
        ),
      ],
    );
    return ListView(
      padding: const EdgeInsets.only(bottom: 16),
      children: [
        if (phone) PageTitle('Contacts', trailing: actions),
        SectionLabel('On your tailnet', trailing: phone ? null : actions),
        if (discovered.isEmpty)
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 2, 20, 8),
            child: Text(
              model.status.hasPeerList
                  ? 'Look for chat peers among the machines Tailscale shows online, or add one by name or address.'
                  : 'Look for chat peers through your contacts, which share the machines their Tailscale shows '
                        'online, or add one by its tailnet name (MagicDNS) or its 100.x address.',
              style: small,
            ),
          ),
        for (final (i, d) in discovered.indexed)
          StaggerIn(
            index: i,
            child: Padding(
              padding: const EdgeInsets.symmetric(horizontal: 12),
              child: Card(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(14, 12, 10, 12),
                  child: Row(
                    children: [
                      Monogram(name: d.probe.name, keyHex: d.probe.key, size: 42),
                      const SizedBox(width: 12),
                      Expanded(
                        child: Column(
                          crossAxisAlignment: CrossAxisAlignment.start,
                          children: [
                            Text(d.probe.name, style: display(17, color: k.ink)),
                            const SizedBox(height: 2),
                            Text(
                              d.via == null
                                  ? '${d.peer.hostName}, ${d.probe.address}'
                                  : '${d.probe.address}, via ${d.via}',
                              style: mono(11.5, color: k.muted),
                            ),
                            Text(d.probe.fingerprint, style: mono(11.5, color: k.muted)),
                          ],
                        ),
                      ),
                      TextButton(
                        style: TextButton.styleFrom(
                          backgroundColor: Color.alphaBlend(k.signal.withValues(alpha: 0.12), k.card),
                        ),
                        onPressed: () => model.addDiscovered(d),
                        child: const Text('Add'),
                      ),
                    ],
                  ),
                ),
              ),
            ),
          ),
        const SizedBox(height: 6),
        const SectionLabel('Contacts'),
        if (contacts.isEmpty)
          Padding(
            padding: const EdgeInsets.fromLTRB(20, 4, 20, 16),
            child: Text('No contacts yet.', style: small),
          ),
        for (final (i, c) in contacts.indexed)
          StaggerIn(
            index: i,
            child: ListTile(
              leading: Monogram(
                name: c.name,
                keyHex: c.key,
                size: 40,
                online: model.connectedTo(c.box),
                warn: c.flaggedKey != null,
              ),
              title: Text(c.name, style: display(16.5, color: k.ink)),
              subtitle: Text(
                c.flaggedKey != null
                    ? 'Key changed: check it before you accept it'
                    : c.key == null
                    ? 'Key not known yet: pinned at the first connection'
                    : fingerprintOfHex(c.key!),
                style: c.flaggedKey != null
                    ? Theme.of(context).textTheme.bodySmall?.copyWith(color: k.danger)
                    : c.key == null
                    ? small
                    : mono(11.5, color: k.muted),
              ),
              trailing: Icon(Icons.chevron_right_rounded, color: k.line),
              onTap: () => Navigator.of(context).push(
                MaterialPageRoute<void>(
                  builder: (_) => ContactPage(model: model, contactId: c.id),
                ),
              ),
            ),
          ),
      ],
    );
  }
}

/// Look for chat peers: while it looks, a sweep goes round, as a radar's.
class _LookButton extends StatefulWidget {
  const _LookButton({required this.looking, required this.onPressed});
  final bool looking;
  final VoidCallback? onPressed;

  @override
  State<_LookButton> createState() => _LookButtonState();
}

class _LookButtonState extends State<_LookButton> with SingleTickerProviderStateMixin {
  late final _c = AnimationController(vsync: this, duration: const Duration(milliseconds: 1400));

  void _follow() {
    if (widget.looking && ambientMotion(context)) {
      if (!_c.isAnimating) _c.repeat();
    } else {
      _c.stop();
    }
  }

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    _follow();
  }

  @override
  void didUpdateWidget(_LookButton old) {
    super.didUpdateWidget(old);
    _follow();
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    if (widget.looking) {
      return SizedBox(
        width: 48,
        height: 48,
        child: Center(
          child: AnimatedBuilder(
            animation: _c,
            builder: (context, _) => CustomPaint(size: const Size(24, 24), painter: _Sweep(_c.value, k.signal)),
          ),
        ),
      );
    }
    return IconButton(
      tooltip: 'Look for chat peers',
      icon: const Icon(Icons.radar_rounded),
      onPressed: widget.onPressed,
    );
  }
}

class _Sweep extends CustomPainter {
  _Sweep(this.t, this.color);
  final double t;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final c = size.center(Offset.zero);
    final r = size.width / 2;
    final ring = Paint()
      ..style = PaintingStyle.stroke
      ..strokeWidth = 1.4
      ..color = color.withValues(alpha: 0.4);
    canvas.drawCircle(c, r, ring);
    canvas.drawCircle(c, r * 0.5, ring);
    final a = t * 2 * math.pi;
    final sweep = Paint()
      ..shader = SweepGradient(
        startAngle: a - 1.2,
        endAngle: a,
        colors: [color.withValues(alpha: 0), color.withValues(alpha: 0.7)],
        transform: GradientRotation(0),
      ).createShader(Rect.fromCircle(center: c, radius: r));
    canvas.drawArc(Rect.fromCircle(center: c, radius: r), a - 1.2, 1.2, true, sweep);
    canvas.drawCircle(c, 2, Paint()..color = color);
  }

  @override
  bool shouldRepaint(_Sweep old) => old.t != t;
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
    content: Column(
      mainAxisSize: MainAxisSize.min,
      crossAxisAlignment: CrossAxisAlignment.start,
      children: [
        Text('Its key is pinned the first time it answers.', style: Theme.of(context).textTheme.bodySmall),
        const SizedBox(height: 14),
        TextField(
          key: const Key('add-by-name'),
          controller: _text,
          autofocus: true,
          style: mono(14, color: Palette.of(context).ink),
          decoration: InputDecoration(
            labelText: 'Tailnet name or address',
            hintText: 'pixel-8, 100.101.102.103, or host:4810',
            errorText: _error,
          ),
          onSubmitted: (_) => _add(),
        ),
      ],
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
      final k = Palette.of(context);
      final connected = model.connectedTo(c.box);
      Widget fact(IconData icon, String label, Widget value) => Padding(
        padding: const EdgeInsets.symmetric(vertical: 8),
        child: Row(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Icon(icon, size: 18, color: k.muted),
            const SizedBox(width: 14),
            Expanded(
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(label.toUpperCase(), style: mono(10.5, color: k.muted, w: 560).copyWith(letterSpacing: 1.3)),
                  const SizedBox(height: 3),
                  value,
                ],
              ),
            ),
          ],
        ),
      );
      return Scaffold(
        appBar: AppBar(title: const Text('')),
        body: Grain(
          child: ListView(
            padding: const EdgeInsets.fromLTRB(20, 20, 20, 32),
            children: [
              Center(
                child: Monogram(name: c.name, keyHex: c.key, size: 88, online: connected, warn: c.flaggedKey != null),
              ),
              const SizedBox(height: 14),
              Center(
                child: Text(
                  c.name,
                  style: display(32, color: k.ink),
                  textAlign: TextAlign.center,
                ),
              ),
              const SizedBox(height: 6),
              Center(
                child: Text(
                  connected ? 'Connected now' : (c.tailnetName ?? c.address?.toString() ?? ''),
                  style: mono(12, color: connected ? k.moss : k.muted),
                ),
              ),
              const SizedBox(height: 22),
              if (c.flaggedKey != null)
                Card(
                  color: Color.alphaBlend(k.danger.withValues(alpha: 0.08), k.card),
                  shape: RoundedRectangleBorder(
                    borderRadius: BorderRadius.circular(18),
                    side: BorderSide(color: k.danger.withValues(alpha: 0.5)),
                  ),
                  child: Padding(
                    padding: const EdgeInsets.all(18),
                    child: Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      children: [
                        Text('Key changed', style: display(20, color: k.danger)),
                        const SizedBox(height: 8),
                        Text(
                          'A chat peer using ${c.name}\'s name or address proved a different key, and was refused. '
                          'That happens when an app is reinstalled or a device loses its identity; it is also what '
                          'an impostor on your tailnet would look like. Compare the new fingerprint with the one on '
                          'its screen before you accept it.',
                          style: Theme.of(context).textTheme.bodyMedium,
                        ),
                        const SizedBox(height: 10),
                        Text('New: ${fingerprintOfHex(c.flaggedKey!)}', style: mono(13, color: k.ink)),
                        const SizedBox(height: 14),
                        FilledButton(
                          onPressed: () => model.acceptNewKey(c.box),
                          child: const Text('Accept the new key'),
                        ),
                      ],
                    ),
                  ),
                ),
              Card(
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(18, 10, 18, 10),
                  child: Column(
                    children: [
                      fact(
                        Icons.fingerprint_rounded,
                        'Fingerprint',
                        Fingerprint(c.key == null ? 'Not known yet' : fingerprintOfHex(c.key!), size: 14),
                      ),
                      if (c.address != null)
                        fact(Icons.lan_outlined, 'Address', Text('${c.address}', style: mono(13, color: k.ink))),
                      if (c.tailnetName != null)
                        fact(Icons.dns_outlined, 'Tailnet name', Text(c.tailnetName!, style: mono(13, color: k.ink))),
                      fact(
                        Icons.how_to_reg_outlined,
                        'Our request',
                        Text(switch ((c.acceptedByThem, c.declinedByThem)) {
                          (_, true) => 'Declined: nothing more is sent',
                          (true, _) => 'Accepted',
                          _ => 'Not answered yet (or they wrote first)',
                        }, style: Theme.of(context).textTheme.bodyMedium),
                      ),
                    ],
                  ),
                ),
              ),
              const SizedBox(height: 18),
              Wrap(
                spacing: 8,
                runSpacing: 8,
                alignment: WrapAlignment.center,
                children: [
                  FilledButton.icon(
                    icon: const Icon(Icons.chat_bubble_rounded, size: 18),
                    label: const Text('Message'),
                    onPressed: () => Navigator.of(context).push(
                      MaterialPageRoute<void>(
                        builder: (_) => ConversationPage(model: model, contactId: c.id),
                      ),
                    ),
                  ),
                  OutlinedButton.icon(
                    icon: const Icon(Icons.edit_outlined, size: 18),
                    label: const Text('Rename'),
                    onPressed: () => _rename(context, c),
                  ),
                  OutlinedButton.icon(
                    style: OutlinedButton.styleFrom(foregroundColor: k.danger),
                    icon: const Icon(Icons.person_remove_outlined, size: 18),
                    label: const Text('Remove'),
                    onPressed: () => _remove(context, c),
                  ),
                ],
              ),
            ],
          ),
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
