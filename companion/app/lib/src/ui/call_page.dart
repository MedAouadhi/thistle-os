import 'dart:async';

import 'package:flutter/material.dart';

import '../chat_model.dart';

bool _showing = false;

/// Show the call screen over whatever is on screen, once.
void showCall(BuildContext context, ChatModel model) {
  if (_showing) return;
  _showing = true;
  Navigator.of(
    context,
  ).push(MaterialPageRoute<void>(fullscreenDialog: true, builder: (_) => CallPage(model: model))).whenComplete(() {
    _showing = false;
    model.talk.dismissCall();
  });
}

/// What a finished call's state says, for the call screen.
String callOutcome(String state) => switch (state) {
  'ended:unreachable' => 'Not reachable now. Leave a voice message instead.',
  'declined:busy' => 'Busy: in another call',
  'declined:declined' => 'Declined',
  'declined:unsupported' => 'Their app cannot take calls',
  'ended:timeout' => 'Call ended: no sound for 10 s',
  'ended:cellular' => 'Call ended: a phone call took over',
  'ended:error' => 'Call ended: the connection dropped',
  _ => 'Call ended',
};

/// The call in progress: who, its state and time, answer and decline for a
/// call coming in, then mute, volume and hang up.
class CallPage extends StatefulWidget {
  const CallPage({super.key, required this.model});
  final ChatModel model;

  @override
  State<CallPage> createState() => _CallPageState();
}

class _CallPageState extends State<CallPage> {
  Timer? _tick;
  Timer? _close;

  @override
  void initState() {
    super.initState();
    _tick = Timer.periodic(const Duration(seconds: 1), (_) {
      if (mounted) setState(() {});
    });
  }

  @override
  void dispose() {
    _tick?.cancel();
    _close?.cancel();
    super.dispose();
  }

  void _closeSoon() {
    _close ??= Timer(const Duration(seconds: 3), () {
      if (mounted) Navigator.of(context).maybePop();
    });
  }

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: widget.model,
    builder: (context, _) {
      final talk = widget.model.talk;
      final call = talk.call;
      final theme = Theme.of(context);
      if (call == null) {
        _closeSoon();
        return const Scaffold(body: Center(child: Text('No call')));
      }
      final name = talk.nameOf(call.peer);
      final state = call.state;
      final ringing = state == 'ringing' && !call.mine;
      final d = talk.connectedFor;
      final status = switch (state) {
        'offered' => 'Calling…',
        'ringing' => 'Calling you',
        'connected' => '${d.inMinutes}:${(d.inSeconds % 60).toString().padLeft(2, '0')}',
        _ => callOutcome(state),
      };
      if (!call.live) _closeSoon();
      return Scaffold(
        appBar: AppBar(title: const Text('Tailnet call'), automaticallyImplyLeading: !call.live),
        body: SafeArea(
          child: Padding(
            padding: const EdgeInsets.all(24),
            child: Column(
              children: [
                const Spacer(),
                CircleAvatar(radius: 40, child: Text(name.isEmpty ? '?' : name[0].toUpperCase())),
                const SizedBox(height: 16),
                Text(name, style: theme.textTheme.headlineSmall),
                const SizedBox(height: 8),
                Text(status, key: const Key('call-status'), style: theme.textTheme.titleMedium),
                if (talk.audioProblem != null)
                  Padding(
                    padding: const EdgeInsets.only(top: 8),
                    child: Text(talk.audioProblem!, style: TextStyle(color: theme.colorScheme.error)),
                  ),
                if (state == 'connected' && talk.echoCancellation != null)
                  Padding(
                    padding: const EdgeInsets.only(top: 8),
                    child: Text('Echo cancellation: ${talk.echoCancellation}', style: theme.textTheme.bodySmall),
                  ),
                const Spacer(),
                if (state == 'connected')
                  Row(
                    children: [
                      const Icon(Icons.volume_down),
                      Expanded(
                        child: Slider(
                          key: const Key('call-volume'),
                          value: talk.volume,
                          max: 2,
                          onChanged: (v) => talk.volume = v,
                        ),
                      ),
                      const Icon(Icons.volume_up),
                    ],
                  ),
                const SizedBox(height: 16),
                Row(
                  mainAxisAlignment: MainAxisAlignment.spaceEvenly,
                  children: [
                    if (ringing) ...[
                      FloatingActionButton.extended(
                        key: const Key('call-decline'),
                        heroTag: 'decline',
                        backgroundColor: Colors.red,
                        foregroundColor: Colors.white,
                        onPressed: talk.decline,
                        icon: const Icon(Icons.call_end),
                        label: const Text('Decline'),
                      ),
                      FloatingActionButton.extended(
                        key: const Key('call-answer'),
                        heroTag: 'answer',
                        backgroundColor: Colors.green,
                        foregroundColor: Colors.white,
                        onPressed: talk.answer,
                        icon: const Icon(Icons.call),
                        label: const Text('Answer'),
                      ),
                    ] else if (call.live) ...[
                      FloatingActionButton(
                        key: const Key('call-mute'),
                        heroTag: 'mute',
                        tooltip: talk.muted ? 'Unmute' : 'Mute',
                        onPressed: state == 'connected' ? () => talk.muted = !talk.muted : null,
                        child: Icon(talk.muted ? Icons.mic_off : Icons.mic),
                      ),
                      FloatingActionButton(
                        key: const Key('call-hangup'),
                        heroTag: 'hangup',
                        tooltip: 'Hang up',
                        backgroundColor: Colors.red,
                        foregroundColor: Colors.white,
                        onPressed: talk.hangUp,
                        child: const Icon(Icons.call_end),
                      ),
                    ] else
                      FilledButton(
                        key: const Key('call-close'),
                        onPressed: () => Navigator.of(context).maybePop(),
                        child: const Text('Close'),
                      ),
                  ],
                ),
                const SizedBox(height: 24),
              ],
            ),
          ),
        ),
      );
    },
  );
}
