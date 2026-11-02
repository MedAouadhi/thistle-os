import 'dart:ui' show ImageFilter;

import 'package:flutter/material.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import '../chat_model.dart';
import '../permissions.dart';
import 'call_page.dart';
import 'contacts_page.dart';
import 'conversation_page.dart';
import 'requests_page.dart';
import 'settings_page.dart';
import 'theme.dart';
import 'widgets.dart';

class ThistleChatApp extends StatelessWidget {
  const ThistleChatApp({super.key, required this.model, this.navigatorKey});

  final ChatModel model;
  final GlobalKey<NavigatorState>? navigatorKey;

  @override
  Widget build(BuildContext context) => MaterialApp(
    title: 'Thistle Chat',
    debugShowCheckedModeBanner: false,
    navigatorKey: navigatorKey,
    theme: inkTheme(Brightness.light),
    darkTheme: inkTheme(Brightness.dark),
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

/// The messages of theirs in `box` not read yet.
int unreadIn(ChatModel model, String box) => model.messages(box).where((m) => !m.mine && m.state == 'received').length;

/// "14:05" today, "Yesterday", else "12 Oct".
String shortWhen(int ts, {DateTime? now}) {
  final t = DateTime.fromMillisecondsSinceEpoch(ts);
  final n = now ?? DateTime.now();
  final today = DateTime(n.year, n.month, n.day);
  final day = DateTime(t.year, t.month, t.day);
  if (day == today) return '${t.hour.toString().padLeft(2, '0')}:${t.minute.toString().padLeft(2, '0')}';
  if (today.difference(day).inDays == 1) return 'Yesterday';
  const months = ['Jan', 'Feb', 'Mar', 'Apr', 'May', 'Jun', 'Jul', 'Aug', 'Sep', 'Oct', 'Nov', 'Dec'];
  return '${t.day} ${months[t.month - 1]}';
}

typedef _Destination = (IconData, IconData, String, int);

class HomePage extends StatefulWidget {
  const HomePage({super.key, required this.model});
  final ChatModel model;

  @override
  State<HomePage> createState() => _HomePageState();
}

class _HomePageState extends State<HomePage> with WidgetsBindingObserver {
  int _tab = 0;

  /// The conversation beside the list, in a window wide enough for both.
  int? _beside;

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

  void _go(int tab) => setState(() => _tab = tab);

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: Listenable.merge([widget.model, ?widget.model.permissions]),
    builder: (context, _) {
      final model = widget.model;
      final width = MediaQuery.sizeOf(context).width;
      final wide = width >= 600;
      final split = width >= 1100;
      final destinations = <_Destination>[
        (Icons.chat_bubble_outline_rounded, Icons.chat_bubble_rounded, 'Conversations', 0),
        (Icons.people_outline_rounded, Icons.people_rounded, 'Contacts', 0),
        (Icons.person_add_alt_outlined, Icons.person_add_alt_1_rounded, 'Requests', model.requests.length),
        (Icons.tune_rounded, Icons.tune_rounded, 'Settings', 0),
      ];
      void open(Contact c) {
        if (split) {
          setState(() => _beside = c.id);
        } else {
          Navigator.of(context).push(
            MaterialPageRoute<void>(
              builder: (_) => ConversationPage(model: model, contactId: c.id),
            ),
          );
        }
      }

      final page = switch (_tab) {
        0 => ConversationsPage(
          model: model,
          onOpen: open,
          selected: split ? _beside : null,
          onFind: () => _go(1),
          title: !wide,
        ),
        1 => ContactsPage(model: model),
        2 => RequestsPage(model: model),
        _ => SettingsPage(model: model),
      };
      final banners = [
        if (model.problem != null) _ProblemBanner(model.problem!),
        if (model.permissions case final permissions?)
          for (final p in permissions.refusals.keys) _RefusalBanner(permissions, p),
      ];
      Widget column(Widget child) => Column(
        children: [
          ...banners,
          Expanded(
            child: AnimatedSwitcher(
              duration: const Duration(milliseconds: 260),
              switchInCurve: Curves.easeOutCubic,
              transitionBuilder: (child, a) => FadeTransition(
                opacity: a,
                child: SlideTransition(
                  position: Tween(begin: const Offset(0, 0.015), end: Offset.zero).animate(a),
                  child: child,
                ),
              ),
              child: KeyedSubtree(key: ValueKey(_tab), child: child),
            ),
          ),
        ],
      );

      if (wide) {
        final beside = _beside != null && model.contacts.any((c) => c.id == _beside) ? _beside : null;
        return Scaffold(
          body: Grain(
            child: SafeArea(
              child: Row(
                children: [
                  _Sidebar(model: model, destinations: destinations, selected: _tab, onSelect: _go),
                  VerticalDivider(width: 1, color: Palette.of(context).line),
                  if (split && _tab == 0) ...[
                    SizedBox(width: 380, child: column(page)),
                    VerticalDivider(width: 1, color: Palette.of(context).line),
                    Expanded(
                      child: beside == null
                          ? const _NothingOpen()
                          : ConversationPage(key: ValueKey(beside), model: model, contactId: beside, embedded: true),
                    ),
                  ] else
                    Expanded(child: column(page)),
                ],
              ),
            ),
          ),
        );
      }
      return Scaffold(
        extendBody: true,
        body: Grain(child: SafeArea(bottom: false, child: column(page))),
        bottomNavigationBar: _FloatingNav(destinations: destinations, selected: _tab, onSelect: _go),
      );
    },
  );
}

Widget _icon(IconData i, int count, Color color) => count == 0
    ? Icon(i, color: color)
    : Badge(
        label: Text('$count'),
        child: Icon(i, color: color),
      );

/// The desktop's column: the name, the four places, and this app's own
/// identity at its foot.
class _Sidebar extends StatelessWidget {
  const _Sidebar({required this.model, required this.destinations, required this.selected, required this.onSelect});
  final ChatModel model;
  final List<_Destination> destinations;
  final int selected;
  final ValueChanged<int> onSelect;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    final listening = model.listening;
    return SizedBox(
      width: 232,
      child: Padding(
        padding: const EdgeInsets.fromLTRB(14, 22, 14, 16),
        child: Column(
          crossAxisAlignment: CrossAxisAlignment.start,
          children: [
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 10),
              child: Row(
                crossAxisAlignment: CrossAxisAlignment.end,
                children: [
                  Flexible(
                    child: Text(
                      'Thistle',
                      maxLines: 1,
                      overflow: TextOverflow.fade,
                      softWrap: false,
                      style: display(30, w: 640, color: k.ink),
                    ),
                  ),
                  const SizedBox(width: 6),
                  Padding(
                    padding: const EdgeInsets.only(bottom: 5),
                    child: Text('chat', style: mono(12, color: k.signal, w: 600)),
                  ),
                ],
              ),
            ),
            const SizedBox(height: 28),
            for (final (i, (icon, on, label, n)) in destinations.indexed)
              _SideItem(
                icon: i == selected ? on : icon,
                label: label,
                count: n,
                selected: i == selected,
                onTap: () => onSelect(i),
              ),
            const Spacer(),
            Container(
              padding: const EdgeInsets.all(14),
              decoration: BoxDecoration(
                color: k.card,
                borderRadius: BorderRadius.circular(16),
                border: Border.all(color: k.line),
              ),
              child: Column(
                crossAxisAlignment: CrossAxisAlignment.start,
                children: [
                  Row(
                    children: [
                      Container(
                        width: 8,
                        height: 8,
                        decoration: BoxDecoration(shape: BoxShape.circle, color: listening == null ? k.muted : k.moss),
                      ),
                      const SizedBox(width: 8),
                      Expanded(
                        child: Text(
                          listening == null ? 'Not listening' : 'Listening',
                          style: Theme.of(context).textTheme.labelMedium,
                        ),
                      ),
                    ],
                  ),
                  const SizedBox(height: 8),
                  Text(
                    model.name,
                    style: display(16, color: k.ink),
                    maxLines: 1,
                    overflow: TextOverflow.ellipsis,
                  ),
                  const SizedBox(height: 2),
                  Text(model.identity.fingerprint.split(' ').take(3).join(' '), style: mono(11.5, color: k.muted)),
                ],
              ),
            ),
          ],
        ),
      ),
    );
  }
}

class _SideItem extends StatelessWidget {
  const _SideItem({
    required this.icon,
    required this.label,
    required this.count,
    required this.selected,
    required this.onTap,
  });
  final IconData icon;
  final String label;
  final int count;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return Padding(
      padding: const EdgeInsets.only(bottom: 4),
      child: Material(
        color: Colors.transparent,
        child: InkWell(
          borderRadius: BorderRadius.circular(14),
          onTap: onTap,
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 220),
            curve: Curves.easeOutCubic,
            padding: const EdgeInsets.symmetric(horizontal: 12, vertical: 11),
            decoration: BoxDecoration(
              color: selected ? k.ink : Colors.transparent,
              borderRadius: BorderRadius.circular(14),
            ),
            child: Row(
              children: [
                Icon(icon, size: 21, color: selected ? k.paper : k.muted),
                const SizedBox(width: 12),
                Expanded(
                  child: Text(
                    label,
                    style: Theme.of(context).textTheme.labelLarge?.copyWith(color: selected ? k.paper : k.ink),
                  ),
                ),
                if (count > 0)
                  Container(
                    padding: const EdgeInsets.symmetric(horizontal: 7, vertical: 2),
                    decoration: BoxDecoration(color: k.signal, borderRadius: BorderRadius.circular(10)),
                    child: Text('$count', style: mono(11, color: k.onSignal, w: 640)),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// The phone's tabs: a pill that floats over the page, blurring what
/// scrolls beneath it; the chosen tab grows to show its name.
class _FloatingNav extends StatelessWidget {
  const _FloatingNav({required this.destinations, required this.selected, required this.onSelect});
  final List<_Destination> destinations;
  final int selected;
  final ValueChanged<int> onSelect;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return SafeArea(
      minimum: const EdgeInsets.fromLTRB(16, 0, 16, 14),
      child: ClipRRect(
        borderRadius: BorderRadius.circular(30),
        child: BackdropFilter(
          filter: ImageFilter.blur(sigmaX: 18, sigmaY: 18),
          child: Container(
            height: 62,
            padding: const EdgeInsets.symmetric(horizontal: 6),
            decoration: BoxDecoration(
              color: k.card.withValues(alpha: 0.78),
              borderRadius: BorderRadius.circular(30),
              border: Border.all(color: k.line),
            ),
            child: Row(
              children: [
                for (final (i, (icon, on, label, n)) in destinations.indexed)
                  Expanded(
                    flex: i == selected ? 5 : 3,
                    child: _NavItem(
                      icon: i == selected ? on : icon,
                      label: label,
                      count: n,
                      selected: i == selected,
                      onTap: () => onSelect(i),
                    ),
                  ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

class _NavItem extends StatelessWidget {
  const _NavItem({
    required this.icon,
    required this.label,
    required this.count,
    required this.selected,
    required this.onTap,
  });
  final IconData icon;
  final String label;
  final int count;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return Semantics(
      button: true,
      selected: selected,
      label: label,
      child: GestureDetector(
        behavior: HitTestBehavior.opaque,
        onTap: onTap,
        child: Center(
          child: AnimatedContainer(
            duration: const Duration(milliseconds: 280),
            curve: Curves.easeOutCubic,
            height: 46,
            padding: const EdgeInsets.symmetric(horizontal: 12),
            decoration: BoxDecoration(
              color: selected ? k.ink : Colors.transparent,
              borderRadius: BorderRadius.circular(23),
            ),
            child: Row(
              mainAxisAlignment: MainAxisAlignment.center,
              children: [
                _icon(icon, count, selected ? k.paper : k.muted),
                Flexible(
                  child: AnimatedSize(
                    duration: const Duration(milliseconds: 280),
                    curve: Curves.easeOutCubic,
                    child: selected
                        ? Padding(
                            padding: const EdgeInsets.only(left: 8),
                            child: Text(
                              label == 'Conversations' ? 'Chats' : label,
                              maxLines: 1,
                              overflow: TextOverflow.fade,
                              softWrap: false,
                              style: Theme.of(context).textTheme.labelLarge?.copyWith(color: k.paper),
                            ),
                          )
                        : const SizedBox.shrink(),
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}

/// A slim strip over the page: what stops chat, in the signal colour.
class _Strip extends StatelessWidget {
  const _Strip({required this.icon, required this.text, this.actions = const []});
  final IconData icon;
  final String text;
  final List<Widget> actions;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return Container(
      margin: const EdgeInsets.fromLTRB(12, 10, 12, 0),
      padding: const EdgeInsets.fromLTRB(14, 10, 6, 10),
      decoration: BoxDecoration(
        color: Color.alphaBlend(k.signal.withValues(alpha: 0.09), k.card),
        borderRadius: BorderRadius.circular(14),
        border: Border.all(color: k.signal.withValues(alpha: 0.35)),
      ),
      child: Row(
        children: [
          Icon(icon, color: k.signal, size: 20),
          const SizedBox(width: 12),
          Expanded(child: Text(text, style: Theme.of(context).textTheme.bodyMedium)),
          ...actions,
        ],
      ),
    );
  }
}

class _ProblemBanner extends StatelessWidget {
  const _ProblemBanner(this.text);
  final String text;

  @override
  Widget build(BuildContext context) => _Strip(icon: Icons.wifi_off_rounded, text: text);
}

/// A permission refused, what that stops, and the way to allow it.
class _RefusalBanner extends StatelessWidget {
  const _RefusalBanner(this.permissions, this.permission);
  final Permissions permissions;
  final AppPermission permission;

  @override
  Widget build(BuildContext context) => _Strip(
    icon: permission == AppPermission.microphone ? Icons.mic_off_rounded : Icons.notifications_off_outlined,
    text: permissions.refusal(permission) ?? '',
    actions: [
      TextButton(onPressed: permissions.openSettings, child: const Text('Open settings')),
      IconButton(
        tooltip: 'Dismiss',
        icon: const Icon(Icons.close_rounded, size: 20),
        onPressed: () => permissions.dismiss(permission),
      ),
    ],
  );
}

/// A large title, as a phone's pages have; the desktop's sidebar names
/// the page instead.
class PageTitle extends StatelessWidget {
  const PageTitle(this.text, {super.key, this.trailing});
  final String text;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) => Padding(
    padding: const EdgeInsets.fromLTRB(22, 18, 12, 4),
    child: Row(
      children: [
        Expanded(child: Text(text, style: Theme.of(context).textTheme.displaySmall)),
        ?trailing,
      ],
    ),
  );
}

class _NothingOpen extends StatelessWidget {
  const _NothingOpen();

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return Center(
      child: Column(
        mainAxisSize: MainAxisSize.min,
        children: [
          Icon(Icons.forum_outlined, size: 40, color: k.line),
          const SizedBox(height: 14),
          Text('Pick a conversation', style: display(22, color: k.muted)),
        ],
      ),
    );
  }
}

class ConversationsPage extends StatelessWidget {
  const ConversationsPage({
    super.key,
    required this.model,
    this.onOpen,
    this.selected,
    this.onFind,
    this.title = false,
  });
  final ChatModel model;
  final void Function(Contact)? onOpen;
  final int? selected;
  final VoidCallback? onFind;

  /// The page's own large title, on a phone.
  final bool title;

  @override
  Widget build(BuildContext context) {
    final list = model.conversations;
    final k = Palette.of(context);
    if (list.isEmpty) {
      return Column(
        crossAxisAlignment: CrossAxisAlignment.stretch,
        children: [
          if (title) const PageTitle('Chats'),
          Expanded(
            child: Center(
              child: Padding(
                padding: const EdgeInsets.all(32),
                child: Column(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(
                      'Quiet on the tailnet.',
                      style: display(26, color: k.ink),
                      textAlign: TextAlign.center,
                    ),
                    const SizedBox(height: 10),
                    Text(
                      'No conversations yet. Find chat peers in Contacts.',
                      style: Theme.of(context).textTheme.bodyMedium?.copyWith(color: k.muted),
                      textAlign: TextAlign.center,
                    ),
                    const SizedBox(height: 22),
                    if (onFind != null)
                      FilledButton.icon(
                        onPressed: onFind,
                        icon: const Icon(Icons.radar_rounded, size: 20),
                        label: const Text('Find chat peers'),
                      ),
                  ],
                ),
              ),
            ),
          ),
        ],
      );
    }
    return ListView(
      padding: const EdgeInsets.only(bottom: 12),
      children: [
        if (title) const PageTitle('Chats'),
        const SizedBox(height: 6),
        for (final (i, (c, last)) in list.indexed)
          StaggerIn(
            index: i,
            child: _ConversationRow(
              model: model,
              contact: c,
              last: last,
              selected: c.id == selected,
              onTap: () => onOpen?.call(c),
            ),
          ),
      ],
    );
  }
}

class _ConversationRow extends StatelessWidget {
  const _ConversationRow({
    required this.model,
    required this.contact,
    required this.last,
    required this.selected,
    required this.onTap,
  });
  final ChatModel model;
  final Contact contact;
  final Message? last;
  final bool selected;
  final VoidCallback onTap;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    final c = contact;
    final theme = Theme.of(context).textTheme;
    final unread = unreadIn(model, c.box);
    final typing = model.isTyping(c.box);
    final flagged = c.flaggedKey != null;
    final preview = flagged
        ? 'Key changed: check it in Contacts'
        : typing
        ? 'typing…'
        : last == null
        ? 'No messages yet'
        : '${last!.mine ? 'You: ' : ''}${last!.kind == 'voice' ? 'Voice message' : last!.text ?? ''}';
    return Padding(
      padding: const EdgeInsets.symmetric(horizontal: 10, vertical: 2),
      child: Material(
        color: selected ? k.card : Colors.transparent,
        shape: RoundedRectangleBorder(
          borderRadius: BorderRadius.circular(18),
          side: BorderSide(color: selected ? k.line : Colors.transparent),
        ),
        child: InkWell(
          borderRadius: BorderRadius.circular(18),
          onTap: onTap,
          child: Padding(
            padding: const EdgeInsets.fromLTRB(10, 10, 14, 10),
            child: Row(
              children: [
                Monogram(name: c.name, keyHex: c.key, online: model.connectedTo(c.box), warn: flagged),
                const SizedBox(width: 12),
                Expanded(
                  child: Column(
                    crossAxisAlignment: CrossAxisAlignment.start,
                    children: [
                      Row(
                        children: [
                          Expanded(
                            child: Text(
                              c.name,
                              style: display(17.5, w: unread > 0 ? 640 : 540, color: k.ink),
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                            ),
                          ),
                          if (last != null)
                            Text(shortWhen(last!.ts), style: mono(11, color: unread > 0 ? k.signal : k.muted)),
                        ],
                      ),
                      const SizedBox(height: 4),
                      Row(
                        children: [
                          if (typing) ...[const TypingDots(), const SizedBox(width: 6)],
                          if (!typing && last?.kind == 'voice') ...[
                            Icon(Icons.graphic_eq_rounded, size: 15, color: k.muted),
                            const SizedBox(width: 4),
                          ],
                          Expanded(
                            child: Text(
                              preview,
                              maxLines: 1,
                              overflow: TextOverflow.ellipsis,
                              style: theme.bodyMedium?.copyWith(
                                color: flagged ? k.danger : (unread > 0 ? k.ink : k.muted),
                                fontVariations: weight(unread > 0 ? 560 : 420),
                              ),
                            ),
                          ),
                          if (unread > 0)
                            Container(
                              margin: const EdgeInsets.only(left: 8),
                              constraints: const BoxConstraints(minWidth: 20),
                              padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                              decoration: BoxDecoration(color: k.signal, borderRadius: BorderRadius.circular(10)),
                              child: Text(
                                '$unread',
                                textAlign: TextAlign.center,
                                style: mono(11, color: k.onSignal, w: 680),
                              ),
                            ),
                        ],
                      ),
                    ],
                  ),
                ),
              ],
            ),
          ),
        ),
      ),
    );
  }
}
