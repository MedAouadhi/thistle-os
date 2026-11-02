import 'package:flutter/material.dart';

import '../chat_model.dart';
import '../permissions.dart';
import 'call_page.dart';
import 'contacts_page.dart';
import 'conversation_page.dart';
import 'requests_page.dart';
import 'settings_page.dart';

class ThistleChatApp extends StatelessWidget {
  const ThistleChatApp({super.key, required this.model, this.navigatorKey});

  final ChatModel model;
  final GlobalKey<NavigatorState>? navigatorKey;

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'Thistle Chat',
    navigatorKey: navigatorKey,
    theme: ThemeData(colorSchemeSeed: const Color(0xFF6B4C9A)),
    darkTheme: ThemeData(colorSchemeSeed: const Color(0xFF6B4C9A), brightness: Brightness.dark),
    home: HomePage(model: model),
  );
}

/// Open a conversation over whatever is on screen (a notification's click).
void openConversation(NavigatorState nav, ChatModel model, String box) {
  final c = model.contacts.where((c) => c.box == box).firstOrNull;
  if (c == null) return;
  nav.push(
    MaterialPageRoute<void>(
      builder: (_) => ConversationPage(model: model, contactId: c.id),
    ),
  );
}

class HomePage extends StatefulWidget {
  const HomePage({super.key, required this.model});
  final ChatModel model;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with WidgetsBindingObserver {
  int _tab = 0;

  String? _shownCall;

  @override
  void initState() {
    super.initState();
    WidgetsBinding.instance.addObserver(this);
    widget.model.addListener(_watchCalls);
  }

  @override
  void dispose() {
    widget.model.removeListener(_watchCalls);
    WidgetsBinding.instance.removeObserver(this);
    super.dispose();
  }

  /// A call coming in shows the call screen over whatever is in front.
  void _watchCalls() {
    final talk = widget.model.talk;
    final call = talk.call;
    if (call == null || talk.callId == _shownCall || call.mine || call.state != 'ringing') return;
    _shownCall = talk.callId;
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted) showCall(context, widget.model);
    });
  }

  /// In front only while resumed: a message to a hidden window notifies.
  @override
  void didChangeAppLifecycleState(AppLifecycleState state) =>
      widget.model.foreground = state == AppLifecycleState.resumed;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: Listenable.merge([widget.model, ?widget.model.permissions]),
    builder: (context, _) {
      final model = widget.model;
      final requests = model.requests.length;
      final destinations = [
        (Icons.chat_bubble_outline, Icons.chat_bubble, 'Conversations', 0),
        (Icons.people_outline, Icons.people, 'Contacts', 0),
        (Icons.person_add_alt, Icons.person_add, 'Requests', requests),
        (Icons.settings_outlined, Icons.settings, 'Settings', 0),
      ];
      Widget icon(IconData i, int count) => count == 0 ? Icon(i) : Badge(label: Text('$count'), child: Icon(i));
      final page = switch (_tab) {
        0 => ConversationsPage(model: model),
        1 => ContactsPage(model: model),
        2 => RequestsPage(model: model),
        _ => SettingsPage(model: model),
      };
      final body = Column(
        children: [
          if (model.problem != null) _ProblemBanner(model.problem!),
          if (model.permissions case final permissions?)
            for (final p in permissions.refusals.keys) _RefusalBanner(permissions, p),
          Expanded(child: page),
        ],
      );
      final wide = MediaQuery.sizeOf(context).width >= 600;
      if (wide) {
        return Scaffold(
          body: SafeArea(
            child: Row(
              children: [
                NavigationRail(
                  selectedIndex: _tab,
                  onDestinationSelected: (i) => setState(() => _tab = i),
                  labelType: NavigationRailLabelType.all,
                  destinations: [
                    for (final (i, s, label, n) in destinations)
                      NavigationRailDestination(icon: icon(i, n), selectedIcon: icon(s, n), label: Text(label)),
                  ],
                ),
                const VerticalDivider(width: 1),
                Expanded(child: body),
              ],
            ),
          ),
        );
      }
      return Scaffold(
        body: SafeArea(child: body),
        bottomNavigationBar: NavigationBar(
          selectedIndex: _tab,
          onDestinationSelected: (i) => setState(() => _tab = i),
          destinations: [
            for (final (i, s, label, n) in destinations)
              NavigationDestination(icon: icon(i, n), selectedIcon: icon(s, n), label: label),
          ],
        ),
      );
    },
  );
}

class _ProblemBanner extends StatelessWidget {
  const _ProblemBanner(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Material(
      color: scheme.errorContainer,
      child: Padding(
        padding: const EdgeInsets.symmetric(horizontal: 16, vertical: 12),
        child: Row(
          children: [
            Icon(Icons.wifi_off, color: scheme.onErrorContainer),
            const SizedBox(width: 12),
            Expanded(
              child: Text(text, style: TextStyle(color: scheme.onErrorContainer)),
            ),
          ],
        ),
      ),
    );
  }
}

/// A permission refused, what that stops, and the way to allow it.
class _RefusalBanner extends StatelessWidget {
  const _RefusalBanner(this.permissions, this.permission);
  final Permissions permissions;
  final AppPermission permission;

  @override
  Widget build(BuildContext context) {
    final scheme = Theme.of(context).colorScheme;
    return Material(
      color: scheme.tertiaryContainer,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(16, 8, 4, 8),
        child: Row(
          children: [
            Icon(
              permission == AppPermission.microphone ? Icons.mic_off : Icons.notifications_off,
              color: scheme.onTertiaryContainer,
            ),
            const SizedBox(width: 12),
            Expanded(
              child: Text(permissions.refusal(permission) ?? '', style: TextStyle(color: scheme.onTertiaryContainer)),
            ),
            TextButton(onPressed: permissions.openSettings, child: const Text('Open settings')),
            IconButton(
              tooltip: 'Dismiss',
              icon: const Icon(Icons.close),
              onPressed: () => permissions.dismiss(permission),
            ),
          ],
        ),
      ),
    );
  }
}

class ConversationsPage extends StatelessWidget {
  const ConversationsPage({super.key, required this.model});
  final ChatModel model;

  @override
  Widget build(BuildContext context) {
    final list = model.conversations;
    if (list.isEmpty) {
      return const Center(child: Text('No conversations yet. Find chat peers in Contacts.'));
    }
    return ListView(
      children: [
        for (final (c, last) in list)
          ListTile(
            leading: CircleAvatar(child: Text(c.name.isEmpty ? '?' : c.name.characters.first.toUpperCase())),
            title: Text(c.name),
            subtitle: Text(
              c.flaggedKey != null
                  ? 'Key changed: check it in Contacts'
                  : model.isTyping(c.box)
                  ? 'typing…'
                  : last == null
                  ? 'No messages yet'
                  : '${last.mine ? 'You: ' : ''}${last.kind == 'voice' ? 'Voice message' : last.text ?? ''}',
              maxLines: 1,
              overflow: TextOverflow.ellipsis,
            ),
            trailing: c.flaggedKey != null ? const Icon(Icons.warning_amber) : null,
            onTap: () => Navigator.of(context).push(
              MaterialPageRoute<void>(
                builder: (_) => ConversationPage(model: model, contactId: c.id),
              ),
            ),
          ),
      ],
    );
  }
}
