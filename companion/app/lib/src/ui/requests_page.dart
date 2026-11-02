import 'package:flutter/material.dart';

import '../chat_model.dart';
import 'app.dart' show PageTitle;
import 'theme.dart';
import 'widgets.dart';

/// Contact requests: a stranger's name, where it wrote from, its key's
/// fingerprint and what it sent, to accept or decline.
class RequestsPage extends StatelessWidget {
  const RequestsPage({super.key, required this.model});
  final ChatModel model;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    final requests = model.requests;
    final phone = MediaQuery.sizeOf(context).width < 600;
    if (requests.isEmpty) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (phone) const PageTitle('Requests'),
          Expanded(
            child: Center(
              child: Column(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.mark_email_read_outlined, size: 38, color: k.line),
                  const SizedBox(height: 12),
                  Text('No contact requests.', style: display(20, color: k.muted)),
                ],
              ),
            ),
          ),
        ],
      );
    }
    return ListView(
      padding: const EdgeInsets.fromLTRB(12, 0, 12, 16),
      children: [
        if (phone) const PageTitle('Requests'),
        const SectionLabel('Asking to talk'),
        for (final (i, r) in requests.indexed)
          StaggerIn(
            index: i,
            child: Card(
              child: Padding(
                padding: const EdgeInsets.all(18),
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  children: [
                    Row(
                      children: [
                        Monogram(name: r.name, keyHex: r.key, size: 44),
                        const SizedBox(width: 12),
                        Expanded(
                          child: Column(
                            crossAxisAlignment: CrossAxisAlignment.start,
                            children: [
                              Text(r.name, style: display(19, color: k.ink)),
                              if (r.host != null) Text('From ${r.host}', style: mono(11.5, color: k.muted)),
                            ],
                          ),
                        ),
                      ],
                    ),
                    const SizedBox(height: 12),
                    Container(
                      width: double.infinity,
                      padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 10),
                      decoration: BoxDecoration(
                        color: k.paper,
                        borderRadius: BorderRadius.circular(12),
                        border: Border.all(color: k.line),
                      ),
                      child: SelectableText('Fingerprint ${r.fingerprint}', style: mono(12, color: k.ink)),
                    ),
                    const SizedBox(height: 12),
                    for (final m in r.held)
                      Container(
                        margin: const EdgeInsets.only(bottom: 6),
                        padding: const EdgeInsets.fromLTRB(12, 8, 12, 8),
                        decoration: BoxDecoration(
                          border: Border(left: BorderSide(color: k.signal.withValues(alpha: 0.6), width: 3)),
                        ),
                        child: Text(
                          '“${m.kind == 'voice' ? 'Voice message' : m.text ?? ''}”',
                          style: display(16, w: 420, color: k.ink, height: 1.35),
                        ),
                      ),
                    const SizedBox(height: 10),
                    Row(
                      children: [
                        FilledButton(onPressed: () => model.accept(r.key), child: const Text('Accept')),
                        const SizedBox(width: 8),
                        OutlinedButton(onPressed: () => model.decline(r.key), child: const Text('Decline')),
                      ],
                    ),
                  ],
                ),
              ),
            ),
          ),
      ],
    );
  }
}
