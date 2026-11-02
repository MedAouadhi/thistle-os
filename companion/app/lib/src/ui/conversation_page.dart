import 'dart:convert';

import 'package:flutter/material.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import '../chat_model.dart';
import 'call_page.dart';

String clock(int ms) {
  final s = ms ~/ 1000;
  return '${s ~/ 60}:${(s % 60).toString().padLeft(2, '0')}';
}

const reactionChoices = ['👍', '❤', '😂', '😮', '😢', '🙏'];

/// One conversation: its messages in the order written, their state,
/// replies and reactions, the other side typing, and the compose field.
class ConversationPage extends StatefulWidget {
  const ConversationPage({super.key, required this.model, required this.contactId});
  final ChatModel model;
  final int contactId;

  @override
  State<ConversationPage> createState() => _ConversationPageState();
}

class _ConversationPageState extends State<ConversationPage> {
  final _compose = TextEditingController();
  final _focus = FocusNode();
  Message? _replyingTo;
  String? _box;

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
    final choice = await showModalBottomSheet<String>(
      context: context,
      builder: (context) => SafeArea(
        child: Column(
          mainAxisSize: MainAxisSize.min,
          children: [
            Padding(
              padding: const EdgeInsets.all(8),
              child: Wrap(
                spacing: 4,
                children: [
                  for (final e in reactionChoices)
                    TextButton(
                      onPressed: () => Navigator.pop(context, e),
                      child: Text(e, style: const TextStyle(fontSize: 24)),
                    ),
                ],
              ),
            ),
            ListTile(
              leading: const Icon(Icons.reply),
              title: const Text('Reply'),
              onTap: () => Navigator.pop(context, 'reply'),
            ),
            if (mine != null)
              ListTile(
                leading: const Icon(Icons.close),
                title: Text('Remove my reaction $mine'),
                onTap: () => Navigator.pop(context, 'unreact'),
              ),
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
      final box = c.box;
      final messages = model.messages(box);
      final status = model.isTyping(box)
          ? 'typing…'
          : c.flaggedKey != null
          ? 'Key changed: check it in Contacts'
          : c.declinedByThem
          ? 'Declined your contact request'
          : model.connectedTo(box)
          ? 'Connected'
          : c.key != null && model.unreachable.containsKey(c.key)
          ? 'Not reachable now; messages wait until it is'
          : '';
      return Scaffold(
        appBar: AppBar(
          title: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            children: [
              Text(c.name),
              if (status.isNotEmpty) Text(status, style: Theme.of(context).textTheme.bodySmall),
            ],
          ),
          actions: [
            IconButton(
              key: const Key('call'),
              tooltip: 'Call',
              icon: const Icon(Icons.call),
              onPressed: c.declinedByThem || (c.key == null && c.address == null) ? null : _call,
            ),
          ],
        ),
        body: Column(
          children: [
            Expanded(
              child: ListView.builder(
                reverse: true,
                padding: const EdgeInsets.all(12),
                itemCount: messages.length,
                itemBuilder: (context, i) {
                  final m = messages[messages.length - 1 - i];
                  return _Bubble(
                    message: m,
                    quoted: m.replyTo == null ? null : model.quoted(box, m.replyTo!),
                    onLongPress: () => _actions(m),
                    playing: model.talk.playingId == m.id,
                    playedMs: model.talk.playingId == m.id ? model.talk.playedMs : 0,
                    onPlay: () => _play(m),
                  );
                },
              ),
            ),
            if (_replyingTo != null)
              Container(
                key: const Key('replying'),
                color: Theme.of(context).colorScheme.surfaceContainerHighest,
                padding: const EdgeInsets.only(left: 16),
                child: Row(
                  children: [
                    const Icon(Icons.reply, size: 18),
                    const SizedBox(width: 8),
                    Expanded(
                      child: Text(_replyingTo!.text ?? 'Voice message', maxLines: 1, overflow: TextOverflow.ellipsis),
                    ),
                    IconButton(
                      tooltip: 'Cancel the reply',
                      icon: const Icon(Icons.close),
                      onPressed: () => setState(() => _replyingTo = null),
                    ),
                  ],
                ),
              ),
            SafeArea(
              top: false,
              child: Padding(
                padding: const EdgeInsets.fromLTRB(12, 4, 4, 8),
                child: model.talk.recordingBox == box
                    ? Row(
                        key: const Key('recording'),
                        children: [
                          const Icon(Icons.fiber_manual_record, color: Colors.red),
                          const SizedBox(width: 8),
                          Expanded(
                            child: Text(
                              'Recording ${clock(model.talk.recordingMs)} of 2:00',
                              key: const Key('recording-time'),
                            ),
                          ),
                          IconButton(
                            key: const Key('recording-cancel'),
                            tooltip: 'Throw it away',
                            icon: const Icon(Icons.delete_outline),
                            onPressed: () => model.talk.stopRecording(send: false),
                          ),
                          IconButton(
                            key: const Key('recording-send'),
                            tooltip: 'Send the voice message',
                            icon: const Icon(Icons.send),
                            onPressed: () => model.talk.stopRecording(),
                          ),
                        ],
                      )
                    : Row(
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
                              decoration: const InputDecoration(hintText: 'Message', border: OutlineInputBorder()),
                              onChanged: (t) => model.typed(box, t),
                              onSubmitted: (_) => _send(),
                            ),
                          ),
                          IconButton(
                            key: const Key('record'),
                            tooltip: 'Record a voice message',
                            icon: const Icon(Icons.mic_none),
                            onPressed: c.declinedByThem || model.talk.recordingBox != null ? null : _record,
                          ),
                          IconButton(tooltip: 'Send', icon: const Icon(Icons.send), onPressed: _send),
                        ],
                      ),
              ),
            ),
          ],
        ),
      );
    },
  );
}

class _Bubble extends StatelessWidget {
  const _Bubble({
    required this.message,
    required this.quoted,
    required this.onLongPress,
    this.playing = false,
    this.playedMs = 0,
    this.onPlay,
  });
  final Message message;
  final Message? quoted;
  final VoidCallback onLongPress;
  final bool playing;
  final int playedMs;
  final VoidCallback? onPlay;

  static String _state(String s) => switch (s) {
    'waiting' => 'Waiting',
    'delivered' => 'Delivered',
    'read' => 'Read',
    _ => '',
  };

  @override
  Widget build(BuildContext context) {
    final m = message;
    final scheme = Theme.of(context).colorScheme;
    final time = DateTime.fromMillisecondsSinceEpoch(m.ts);
    final hhmm = '${time.hour.toString().padLeft(2, '0')}:${time.minute.toString().padLeft(2, '0')}';
    final small = Theme.of(context).textTheme.bodySmall;
    return Align(
      alignment: m.mine ? Alignment.centerRight : Alignment.centerLeft,
      child: GestureDetector(
        key: Key('message-${m.id}'),
        onLongPress: onLongPress,
        onSecondaryTap: onLongPress,
        child: Container(
          constraints: const BoxConstraints(maxWidth: 480),
          margin: const EdgeInsets.symmetric(vertical: 4),
          padding: const EdgeInsets.all(10),
          decoration: BoxDecoration(
            color: m.mine ? scheme.primaryContainer : scheme.surfaceContainerHigh,
            borderRadius: BorderRadius.circular(12),
          ),
          child: Column(
            crossAxisAlignment: CrossAxisAlignment.start,
            mainAxisSize: MainAxisSize.min,
            children: [
              if (m.replyTo != null)
                Container(
                  key: Key('quote-${m.id}'),
                  margin: const EdgeInsets.only(bottom: 6),
                  padding: const EdgeInsets.only(left: 8),
                  decoration: BoxDecoration(
                    border: Border(left: BorderSide(color: scheme.primary, width: 3)),
                  ),
                  child: Text(
                    quoted == null ? 'A message not here' : quoted!.text ?? 'Voice message',
                    maxLines: 2,
                    overflow: TextOverflow.ellipsis,
                    style: small,
                  ),
                ),
              if (m.kind == 'voice')
                Row(
                  mainAxisSize: MainAxisSize.min,
                  children: [
                    IconButton(
                      key: Key('play-${m.id}'),
                      tooltip: playing ? 'Stop' : 'Play',
                      icon: Icon(playing ? Icons.stop : Icons.play_arrow),
                      onPressed: onPlay,
                    ),
                    SizedBox(
                      width: 120,
                      child: LinearProgressIndicator(value: playing && (m.ms ?? 0) > 0 ? playedMs / m.ms! : 0),
                    ),
                    const SizedBox(width: 8),
                    Text(
                      playing ? '${clock(playedMs)} / ${clock(m.ms ?? 0)}' : 'Voice message, ${clock(m.ms ?? 0)}',
                      key: Key('voice-${m.id}'),
                    ),
                  ],
                )
              else
                Text(m.text ?? ''),
              const SizedBox(height: 2),
              Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Text(hhmm, style: small),
                  if (m.mine) ...[const SizedBox(width: 8), Text(_state(m.state), style: small)],
                ],
              ),
              if (m.reactions.isNotEmpty)
                Padding(
                  padding: const EdgeInsets.only(top: 4),
                  child: Wrap(
                    spacing: 4,
                    children: [
                      for (final MapEntry(key: who, value: emoji) in m.reactions.entries)
                        Container(
                          key: Key('reaction-$who'),
                          padding: const EdgeInsets.symmetric(horizontal: 6, vertical: 2),
                          decoration: BoxDecoration(color: scheme.surface, borderRadius: BorderRadius.circular(10)),
                          child: Text(who == 'me' ? '$emoji you' : emoji),
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
