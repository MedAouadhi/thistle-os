import 'package:flutter/material.dart';

import '../chat_model.dart';

/// Contact requests: a stranger's name, where it wrote from, its key's
/// fingerprint and what it sent, to accept or decline.
class RequestsPage extends StatelessWidget {
  const RequestsPage({super.key, required this.model});
  final ChatModel model;

  @override
  Widget build(BuildContext context) {
    final requests = model.requests;
    if (requests.isEmpty) return const Center(child: Text('No contact requests.'));
    final small = Theme.of(context).textTheme.bodySmall;
    return ListView(
      padding: const EdgeInsets.all(8),
      children: [
        for (final r in requests)
          Card(
            child: Padding(
              padding: const EdgeInsets.all(16),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Text(r.name, style: Theme.of(context).textTheme.titleMedium),
                  const SizedBox(height: 4),
                  SelectableText('Fingerprint ${r.fingerprint}', style: small),
                  if (r.host != null) Text('From ${r.host}', style: small),
                  const SizedBox(height: 8),
                  for (final m in r.held)
                    Padding(
                      padding: const EdgeInsets.only(bottom: 4),
                      child: Text('“${m.kind == 'voice' ? 'Voice message' : m.text ?? ''}”'),
                    ),
                  const SizedBox(height: 8),
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
      ],
    );
  }
}
