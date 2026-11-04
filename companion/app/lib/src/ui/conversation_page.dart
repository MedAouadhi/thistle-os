import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import '../chat_model.dart';
import 'call_page.dart';
import 'theme.dart';
import 'widgets.dart';

String clock(int ms) {
  final s = ms ~/ 1000;
  return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
}

const reactionChoices = ['👍', '❤', '😂', '😮', '😢', '🙏'];

/// "Today", "Yesterday", else "Monday 12 October".
String dayLabel(DateTime d, {DateTime? now}) {
  final n = now ?? DateTime.now();
  final today = DateTime(n.year, n.month, n.day);
  final day = DateTime(d.year, d.month, d.day);
  final ago = today.difference(day).inDays;
  if (ago == 0) return 'Today';
  if (ago == 1) return 'Yesterday';
  const days = ['Monday', 'Tuesday', 'Wednesday', 'Thursday', 'Friday', 'Saturday', 'Sunday'];
  const months = [
    'January',
    'February',
    'March',
    'April',
    'May',
    'June',
    'July',
    'August',
    'September',
    'October',
    'November',
    'December',
  ];
  return '${days[d.weekday - 1]} ${d.day} ${months[d.month - 1]}${d.year == n.year ? '' : ' ${d.year}'}';
}

/// One conversation: its messages in the order written, their state,
/// replies and reactions, the other side typing, and the compose field.
class ConversationPage extends StatefulWidget {
  const ConversationPage({super.key, required this.model, required this.contactId, this.embedded = false});
  final ChatModel model;
  final int contactId;

  /// Beside the list in a wide window, rather than a page of its own.
  final bool embedded;

  @override
  State<ConversationPage> createState() => _ConversationPageState();
}

class _ConversationPageState extends State<ConversationPage> {
  final _compose = TextEditingController();
  final _focus = FocusNode();
  Message? _replyingTo;
  String? _box;

  /// The messages there when the page opened: only later ones pop in.
  Set<String>? _seen;

  ChatModel get model => widget.model;
  Contact? get contact => model.contacts.where((c) => c.id == widget.contactId).firstOrNull;

  @override
  void initState() {
    super.initState();
    _box = contact?.box;
    // After this frame: opening reads the conversation, which notifies.
    WidgetsBinding.instance.addPostFrameCallback((_) {
      if (mounted && _box != null) model.openConversation(_box!);
    });
    model.addListener(_followBox);
  }

  /// A contact added by address gets its key's box at its first handshake.
  void _followBox() {
    final box = contact?.box;
    if (box != null && box != _box) {
      _box = box;
      model.openConversation(box);
    }
  }

  @override
  void dispose() {
    model.removeListener(_followBox);
    model.closeConversation();
    _compose.dispose();
    _focus.dispose();
    super.dispose();
  }

  void _send() {
    final text = _compose.text.trim();
    final box = _box;
    if (text.isEmpty || box == null) return;
    if (utf8.encode(text).length > maxTextBytes) {
      ScaffoldMessenger.of(
        context,
      ).showSnackBar(const SnackBar(content: Text('Too long: a message is at most 4096 bytes. Split it.')));
      return;
    }
    model.send(box, text, replyTo: _replyingTo?.id);
    _compose.clear();
    setState(() => _replyingTo = null);
    _focus.requestFocus();
  }

  void _say(String why) => ScaffoldMessenger.of(context).showSnackBar(SnackBar(content: Text(why)));

  Future<void> _record() async {
    final why = await model.talk.startRecording(_box!);
    if (why != null && mounted) _say(why);
  }

  Future<void> _call() async {
    final id = await model.talk.startCall(_box!);
    if (!mounted) return;
    if (id == null) {
      _say(model.talk.audioProblem ?? 'A call is already up.');
      return;
    }
    showCall(context, model);
  }

  Future<void> _play(Message m) async {
    if (model.talk.playingId == m.id) return model.talk.stopPlaying();
    final why = await model.talk.play(m);
    if (why != null && mounted) _say(why);
  }

  Future<void> _actions(Message m) async {
    final box = _box!;
    final mine = m.reactions['me'];
    final k = Palette.of(context);
    final choice = await showModalBottomSheet<String>(
      context: context,
      builder: (context) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Padding(
              padding: const EdgeInsets.fromLTRB(16, 0, 16, 8),
              child: Row(
                mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                children: [
                  for (final (i, e) in reactionChoices.indexed)
                    StaggerIn(
                      index: i,
                      child: TextButton(
                        style: TextButton.styleFrom(
                          shape: const CircleBorder(),
                          padding: const EdgeInsets.all(10),
                          backgroundColor: e == mine ? k.ink.withValues(alpha: 0.08) : null,
                        ),
                        onPressed: () => Navigator.pop(context, e),
                        child: Text(e, style: const TextStyle(fontSize: 28)),
                      ),
                    ),
                ],
              ),
            ),
            Divider(indent: 20, endIndent: 20, color: k.line),
            ListTile(
              leading: const Icon(Icons.reply_rounded),
              title: const Text('Reply'),
              onTap: () => Navigator.pop(context, 'reply'),
            ),
            if (mine != null)
              ListTile(
                leading: const Icon(Icons.close_rounded),
                title: Text('Remove my reaction $mine'),
                onTap: () => Navigator.pop(context, 'unreact'),
              ),
            const SizedBox(height: 8),
          ],
        ),
      ),
    );
    if (choice == null || !mounted) return;
    if (choice == 'reply') {
      setState(() => _replyingTo = m);
      _focus.requestFocus();
    } else {
      model.react(box, m.id, choice == 'unreact' ? '' : choice);
    }
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: model,
    builder: (context, _) {
      final c = contact;
      if (c == null) return const Scaffold(body: Center(child: Text('This contact was removed.')));
      final k = Palette.of(context);
      final box = c.box;
      final messages = model.messages(box);
      _seen ??= {for (final m in messages) m.id};
      final typing = model.isTyping(box);
      final connected = model.connectedTo(box);
      final status = typing
          ? 'typing…'
          : c.flaggedKey != null
          ? 'Key changed: check it in Contacts'
          : c.declinedByThem
          ? 'Declined your contact request'
          : connected
          ? 'Connected'
          : c.key != null && model.unreachable.containsKey(c.key)
          ? 'Not reachable now; messages wait until it is'
          : '';
      final statusColor = typing
          ? k.signal
          : connected
          ? k.moss
          : c.flaggedKey != null
          ? k.danger
          : k.muted;

      // Newest first, as the list is reversed; a day's label above its first.
      final items = <Widget>[];
      for (var i = messages.length - 1; i >= 0; i--) {
        final m = messages[i];
        final prev = i > 0 ? messages[i - 1] : null;
        final next = i < messages.length - 1 ? messages[i + 1] : null;
        final t = DateTime.fromMillisecondsSinceEpoch(m.ts);
        bool sameRun(Message? o) =>
            o != null && o.mine == m.mine && (o.ts - m.ts).abs() < 3 * 60 * 1000 && sameDay(o.ts, m.ts);
        final bubble = _Bubble(
          message: m,
          quoted: m.replyTo == null ? null : model.quoted(box, m.replyTo!),
          onLongPress: () => _actions(m),
          playing: model.talk.playingId == m.id,
          playedMs: model.talk.playingId == m.id ? model.talk.playedMs : 0,
          onPlay: () => _play(m),
          joinsAbove: sameRun(prev),
          joinsBelow: sameRun(next),
        );
        items.add(
          _seen!.contains(m.id)
              ? bubble
              : PopIn(alignment: m.mine ? Alignment.bottomRight : Alignment.bottomLeft, child: bubble),
        );
        if (prev == null || !sameDay(prev.ts, m.ts)) items.add(_DayLabel(dayLabel(t)));
      }

      return Scaffold(
        appBar: AppBar(
          automaticallyImplyLeading: !widget.embedded,
          titleSpacing: widget.embedded ? 20 : 0,
          toolbarHeight: 66,
          title: Row(
            children: [
              Monogram(name: c.name, keyHex: c.key, size: 38, online: connected, warn: c.flaggedKey != null),
              const SizedBox(width: 12),
              Expanded(
                child: Column(
                  crossAxisAlignment: CrossAxisAlignment.start,
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    Text(c.name, maxLines: 1, overflow: TextOverflow.ellipsis),
                    AnimatedSwitcher(
                      duration: const Duration(milliseconds: 200),
                      child: status.isEmpty
                          ? const SizedBox.shrink()
                          : Row(
                              key: ValueKey(status),
                              mainAxisSize: MainAxisSize.min,
                              children: [
                                if (typing) ...[const TypingDots(), const SizedBox(width: 6)],
                                Flexible(
                                  child: Text(
                                    status,
                                    maxLines: 1,
                                    overflow: TextOverflow.ellipsis,
                                    style: mono(11.5, color: statusColor),
                                  ),
                                ),
                              ],
                            ),
                    ),
                  ],
                ),
              ),
            ],
          ),
          actions: [
            Padding(
              padding: const EdgeInsets.only(right: 10),
              child: IconButton.filledTonal(
                key: const Key('call'),
                tooltip: 'Call',
                style: IconButton.styleFrom(
                  backgroundColor: k.card,
                  side: BorderSide(color: k.line),
                ),
                icon: Icon(Icons.call_rounded, color: k.ink),
                onPressed: c.declinedByThem || (c.key == null && c.address == null) ? null : _call,
              ),
            ),
          ],
        ),
        body: Grain(
          child: Column(
            children: [
              Expanded(
                child: messages.isEmpty
                    ? Center(
                        child: Column(
                          mainAxisSize: MainAxisSize.min,
                          children: [
                            Monogram(name: c.name, keyHex: c.key, size: 64),
                            const SizedBox(height: 14),
                            Text('Say hello to ${c.name}', style: display(22, color: k.ink)),
                            const SizedBox(height: 6),
                            Text(
                              'End to end over your tailnet, nothing in between.',
                              style: Theme.of(context).textTheme.bodySmall,
                            ),
                          ],
                        ),
                      )
                    : ListView(reverse: true, padding: const EdgeInsets.fromLTRB(14, 12, 14, 12), children: items),
              ),
              AnimatedSize(
                duration: const Duration(milliseconds: 200),
                curve: Curves.easeOutCubic,
                child: _replyingTo == null
                    ? const SizedBox(width: double.infinity)
                    : _ReplyBar(message: _replyingTo!, onCancel: () => setState(() => _replyingTo = null)),
              ),
              SafeArea(
                top: false,
                child: Padding(
                  padding: const EdgeInsets.fromLTRB(12, 6, 12, 10),
                  child: AnimatedSwitcher(
                    duration: const Duration(milliseconds: 240),
                    switchInCurve: Curves.easeOutCubic,
                    child: model.talk.recordingBox == box ? _recording(context) : _composer(context, c),
                  ),
                ),
              ),
            ],
          ),
        ),
      );
    },
  );

  Widget _recording(BuildContext context) {
    final k = Palette.of(context);
    return Container(
      key: const Key('recording'),
      padding: const EdgeInsets.fromLTRB(16, 4, 4, 4),
      decoration: BoxDecoration(
        color: Color.alphaBlend(k.signal.withValues(alpha: 0.10), k.card),
        borderRadius: BorderRadius.circular(28),
        border: Border.all(color: k.signal.withValues(alpha: 0.45)),
      ),
      child: Row(
        children: [
          const _RecordingDot(),
          const SizedBox(width: 12),
          Expanded(
            child: Text(
              'Recording ${clock(model.talk.recordingMs)} of 2:00',
              key: const Key('recording-time'),
              style: mono(13.5, color: k.ink, w: 520),
            ),
          ),
          IconButton(
            key: const Key('recording-cancel'),
            tooltip: 'Throw it away',
            icon: const Icon(Icons.delete_outline_rounded),
            onPressed: () => model.talk.stopRecording(send: false),
          ),
          IconButton.filled(
            key: const Key('recording-send'),
            tooltip: 'Send the voice message',
            style: IconButton.styleFrom(backgroundColor: k.signal, foregroundColor: k.onSignal),
            icon: const Icon(Icons.arrow_upward_rounded),
            onPressed: () => model.talk.stopRecording(),
          ),
        ],
      ),
    );
  }

  Widget _composer(BuildContext context, Contact c) {
    final k = Palette.of(context);
    final box = c.box;
    return Container(
      key: const Key('composer'),
      padding: const EdgeInsets.fromLTRB(6, 4, 4, 4),
      decoration: BoxDecoration(
        color: k.card,
        borderRadius: BorderRadius.circular(28),
        border: Border.all(color: k.line),
        boxShadow: [BoxShadow(color: Colors.black.withValues(alpha: 0.05), blurRadius: 18, offset: const Offset(0, 6))],
      ),
      child: ValueListenableBuilder<TextEditingValue>(
        valueListenable: _compose,
        builder: (context, value, _) {
          final empty = value.text.trim().isEmpty;
          return Row(
            crossAxisAlignment: CrossAxisAlignment.end,
            children: [
              Expanded(
                child: TextField(
                  key: const Key('compose'),
                  controller: _compose,
                  focusNode: _focus,
                  enabled: !c.declinedByThem,
                  minLines: 1,
                  maxLines: 5,
                  textInputAction: TextInputAction.send,
                  style: Theme.of(context).textTheme.bodyLarge,
                  decoration: const InputDecoration(
                    hintText: 'Message',
                    filled: false,
                    border: InputBorder.none,
                    enabledBorder: InputBorder.none,
                    focusedBorder: InputBorder.none,
                    disabledBorder: InputBorder.none,
                    contentPadding: EdgeInsets.fromLTRB(14, 12, 8, 12),
                  ),
                  onChanged: (t) => model.typed(box, t),
                  onSubmitted: (_) => _send(),
                ),
              ),
              if (empty)
                IconButton(
                  key: const Key('record'),
                  tooltip: 'Record a voice message',
                  icon: Icon(Icons.mic_none_rounded, color: k.muted),
                  onPressed: c.declinedByThem || model.talk.recordingBox != null ? null : _record,
                ),
              AnimatedScale(
                scale: empty ? 0.82 : 1,
                duration: const Duration(milliseconds: 220),
                curve: Curves.easeOutBack,
                child: AnimatedContainer(
                  duration: const Duration(milliseconds: 220),
                  decoration: BoxDecoration(shape: BoxShape.circle, color: empty ? k.line : k.signal),
                  child: IconButton(
                    tooltip: 'Send',
                    icon: Icon(Icons.arrow_upward_rounded, color: empty ? k.muted : k.onSignal),
                    onPressed: _send,
                  ),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

bool sameDay(int a, int b) {
  final x = DateTime.fromMillisecondsSinceEpoch(a), y = DateTime.fromMillisecondsSinceEpoch(b);
  return x.year == y.year && x.month == y.month && x.day == y.day;
}

class _DayLabel extends StatelessWidget {
  const _DayLabel(this.text);
  final String text;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return Padding(
      padding: const EdgeInsets.symmetric(vertical: 14),
      child: Row(
        children: [
          Expanded(child: Container(height: 1, color: k.line)),
          Padding(
            padding: const EdgeInsets.symmetric(horizontal: 12),
            child: Text(text.toUpperCase(), style: mono(10.5, color: k.muted, w: 560).copyWith(letterSpacing: 1.4)),
          ),
          Expanded(child: Container(height: 1, color: k.line)),
        ],
      ),
    );
  }
}

class _ReplyBar extends StatelessWidget {
  const _ReplyBar({required this.message, required this.onCancel});
  final Message message;
  final VoidCallback onCancel;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return Container(
      key: const Key('replying'),
      margin: const EdgeInsets.fromLTRB(12, 4, 12, 0),
      padding: const EdgeInsets.only(left: 14),
      decoration: BoxDecoration(
        color: k.card,
        borderRadius: BorderRadius.circular(16),
        border: Border(left: BorderSide(color: k.signal, width: 3)),
      ),
      child: Row(
        children: [
          Expanded(
            child: Column(
              crossAxisAlignment: CrossAxisAlignment.start,
              mainAxisSize: MainAxisSize.min,
              children: [
                Text(
                  message.mine ? 'REPLYING TO YOURSELF' : 'REPLYING',
                  style: mono(10, color: k.signal, w: 600).copyWith(letterSpacing: 1.2),
                ),
                const SizedBox(height: 2),
                Text(
                  message.text ?? 'Voice message',
                  maxLines: 1,
                  overflow: TextOverflow.ellipsis,
                  style: Theme.of(context).textTheme.bodyMedium,
                ),
              ],
            ),
          ),
          IconButton(tooltip: 'Cancel the reply', icon: const Icon(Icons.close_rounded, size: 20), onPressed: onCancel),
        ],
      ),
    );
  }
}

class _RecordingDot extends StatefulWidget {
  const _RecordingDot();

  @override
  State<_RecordingDot> createState() => _RecordingDotState();
}

class _RecordingDotState extends State<_RecordingDot> with SingleTickerProviderStateMixin {
  late final _c = AnimationController(vsync: this, duration: const Duration(milliseconds: 900));

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (ambientMotion(context)) _c.repeat(reverse: true);
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return AnimatedBuilder(
      animation: _c,
      builder: (context, _) => Container(
        width: 12,
        height: 12,
        decoration: BoxDecoration(
          shape: BoxShape.circle,
          color: k.signal,
          boxShadow: [BoxShadow(color: k.signal.withValues(alpha: 0.5 * _c.value), blurRadius: 10, spreadRadius: 3)],
        ),
      ),
    );
  }
}

class _Bubble extends StatelessWidget {
  const _Bubble({
    required this.message,
    required this.quoted,
    required this.onLongPress,
    this.playing = false,
    this.playedMs = 0,
    this.onPlay,
    this.joinsAbove = false,
    this.joinsBelow = false,
  });
  final Message message;
  final Message? quoted;
  final VoidCallback onLongPress;
  final bool playing;
  final int playedMs;
  final VoidCallback? onPlay;

  /// Next to a message of the same side a moment before or after: the
  /// corners between them are tighter, and so is the gap.
  final bool joinsAbove;
  final bool joinsBelow;

  static String _state(String s) => switch (s) {
    'waiting' => 'Waiting',
    'delivered' => 'Delivered',
    'read' => 'Read',
    _ => '',
  };

  static IconData? _stateIcon(String s) => switch (s) {
    'waiting' => Icons.schedule_rounded,
    'delivered' => Icons.done_rounded,
    'read' => Icons.done_all_rounded,
    _ => null,
  };

  @override
  Widget build(BuildContext context) {
    final m = message;
    final k = Palette.of(context);
    final fg = m.mine ? k.onMine : k.ink;
    final faint = fg.withValues(alpha: 0.6);
    final time = DateTime.fromMillisecondsSinceEpoch(m.ts);
    final hhmm = '${time.hour.toString().padLeft(2, '0')}:${time.minute.toString().padLeft(2, '0')}';
    const big = Radius.circular(20), small = Radius.circular(6);
    final radius = m.mine
        ? BorderRadius.only(
            topLeft: big,
            bottomLeft: big,
            topRight: joinsAbove ? small : big,
            bottomRight: joinsBelow ? small : const Radius.circular(8),
          )
        : BorderRadius.only(
            topRight: big,
            bottomRight: big,
            topLeft: joinsAbove ? small : big,
            bottomLeft: joinsBelow ? small : const Radius.circular(8),
          );
    final stateIcon = m.mine ? _stateIcon(m.state) : null;
    final body = Theme.of(context).textTheme.bodyLarge?.copyWith(color: fg);
    return Align(
      alignment: m.mine ? Alignment.centerRight : Alignment.centerLeft,
      child: GestureDetector(
        key: Key('message-${m.id}'),
        onLongPress: onLongPress,
        onSecondaryTap: onLongPress,
        child: Container(
          constraints: const BoxConstraints(maxWidth: 520),
          margin: EdgeInsets.only(
            top: joinsAbove ? 2 : 6,
            bottom: joinsBelow ? 2 : 6,
            left: m.mine ? 48 : 0,
            right: m.mine ? 0 : 48,
          ),
          padding: const EdgeInsets.fromLTRB(14, 10, 12, 8),
          decoration: BoxDecoration(
            color: m.mine ? k.mine : k.card,
            borderRadius: radius,
            border: m.mine ? null : Border.all(color: k.line),
            boxShadow: m.mine
                ? [BoxShadow(color: Colors.black.withValues(alpha: 0.10), blurRadius: 10, offset: const Offset(0, 3))]
                : null,
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              if (m.replyTo != null)
                Container(
                  key: Key('quote-${m.id}'),
                  margin: const EdgeInsets.only(bottom: 8),
                  padding: const EdgeInsets.fromLTRB(10, 6, 10, 6),
                  decoration: BoxDecoration(
                    color: fg.withValues(alpha: 0.07),
                    borderRadius: BorderRadius.circular(10),
                    border: Border(left: BorderSide(color: k.signal, width: 3)),
                  ),
                  child: Text(
                    quoted == null ? 'A message not here' : quoted!.text ?? 'Voice message',
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: Theme.of(context).textTheme.bodySmall?.copyWith(color: faint),
                  ),
                ),
              if (m.kind == 'voice')
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    IconButton.filled(
                      key: Key('play-${m.id}'),
                      tooltip: playing ? 'Stop' : 'Play',
                      style: IconButton.styleFrom(
                        backgroundColor: m.mine ? k.onMine : k.ink,
                        foregroundColor: m.mine ? k.mine : k.paper,
                        minimumSize: const Size(38, 38),
                      ),
                      icon: Icon(playing ? Icons.stop_rounded : Icons.play_arrow_rounded, size: 22),
                      onPressed: onPlay,
                    ),
                    const SizedBox(width: 10),
                    Column(
                      crossAxisAlignment: CrossAxisAlignment.start,
                      mainAxisSize: MainAxisSize.min,
                      children: [
                        Waveform(
                          seed: m.id,
                          progress: playing && (m.ms ?? 0) > 0 ? playedMs / m.ms! : 0,
                          color: m.mine ? k.onMine : k.signal,
                        ),
                        Text(
                          playing ? '${clock(playedMs)} / ${clock(m.ms ?? 0)}' : 'Voice message, ${clock(m.ms ?? 0)}',
                          key: Key('voice-${m.id}'),
                          style: mono(11, color: faint),
                        ),
                      ],
                    ),
                  ],
                )
              else
                Text(m.text ?? '', style: body),
              const SizedBox(height: 4),
              Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(hhmm, style: mono(10.5, color: faint)),
                  if (m.mine) ...[
                    const SizedBox(width: 8),
                    if (stateIcon != null) ...[
                      Icon(stateIcon, size: 13, color: m.state == 'read' ? k.signal : faint),
                      const SizedBox(width: 3),
                    ],
                    Text(_state(m.state), style: mono(10.5, color: m.state == 'read' ? k.signal : faint)),
                  ],
                ],
              ),
              if (m.reactions.isNotEmpty)
                Padding(
                  padding: const EdgeInsets.only(top: 6),
                  child: Wrap(
                    spacing: 4,
                    children: [
                      for (final MapEntry(key: who, value: emoji) in m.reactions.entries)
                        PopIn(
                          alignment: Alignment.center,
                          child: Container(
                            key: Key('reaction-$who'),
                            padding: const EdgeInsets.symmetric(horizontal: 8, vertical: 3),
                            decoration: BoxDecoration(
                              color: m.mine ? k.onMine.withValues(alpha: 0.14) : k.paper,
                              borderRadius: BorderRadius.circular(12),
                              border: Border.all(color: m.mine ? Colors.transparent : k.line),
                            ),
                            child: Text(who == 'me' ? '$emoji you' : emoji, style: TextStyle(fontSize: 13, color: fg)),
                          ),
                        ),
                    ],
                  ),
                ),
            ],
          ),
        ),
      ),
    );
  }
}
