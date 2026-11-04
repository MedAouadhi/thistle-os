import 'dart:async';

import 'package:flutter/material.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart' show Call, CameraState, transportWebrtc;

import '../chat_model.dart';
import 'theme.dart';
import 'widgets.dart';

/// The call screen showCall pushed, while it is up.
Route<void>? _showing;

/// Android's call window over the lock screen (CallActivity.kt), on the
/// app's one engine: while it shows, the app draws the call screen and
/// nothing of the chats, and closing the call screen finishes the window,
/// so a locked phone is back at its lock screen.
abstract interface class CallWindow implements Listenable {
  bool get shown;
  void finish();
}

/// What the app draws in its window: the screens, or while [window] shows,
/// the call alone, the screens kept but neither drawn nor reachable.
class CallWindowed extends StatelessWidget {
  const CallWindowed({super.key, required this.model, required this.window, required this.child});
  final ChatModel model;
  final CallWindow window;
  final Widget child;

  @override
  Widget build(BuildContext context) => ListenableBuilder(
    listenable: window,
    builder: (context, _) => Stack(
      children: [
        Offstage(
          offstage: window.shown,
          child: TickerMode(enabled: !window.shown, child: child),
        ),
        if (window.shown)
          // A navigator of its own for the call screen's dialogs and
          // tooltips, with nothing beneath to go back to.
          HeroControllerScope.none(
            child: Navigator(
              onGenerateRoute: (_) => MaterialPageRoute<void>(
                builder: (_) => CallPage(
                  model: model,
                  onClose: () {
                    model.talk.dismissCall();
                    window.finish();
                  },
                ),
              ),
            ),
          ),
      ],
    ),
  );
}

/// Show the call screen over whatever is on screen, once.
void showCall(BuildContext context, ChatModel model) {
  // Gone with its navigator too, which completes nothing.
  if (_showing?.isActive ?? false) return;
  final route = _showing = MaterialPageRoute<void>(fullscreenDialog: true, builder: (_) => CallPage(model: model));
  Navigator.of(context).push(route).whenComplete(() {
    if (_showing == route) _showing = null;
    model.talk.dismissCall();
  });
}

/// What the other side's tile says while its camera gives no picture.
const remoteCameraTile = <CameraState, String>{
  CameraState.off: 'Camera off',
  CameraState.paused: 'Camera paused',
  CameraState.none: 'No camera',
};

/// What a finished call's state says, for the call screen; on WebRTC, a
/// timeout is media that never connected.
String callOutcome(String state, {String? transport}) => switch (state) {
  'ended:unreachable' => 'Not reachable now. Leave a voice message instead.',
  'declined:busy' => 'Busy: in another call',
  'declined:declined' => 'Declined',
  'declined:unsupported' => 'Their app cannot take calls',
  'ended:timeout' when transport == transportWebrtc => 'Call ended: it could not connect',
  'ended:timeout' => 'Call ended: no sound for 10 s',
  'ended:cellular' => 'Call ended: a phone call took over',
  'ended:error' => 'Call ended: the connection dropped',
  _ => 'Call ended',
};

/// The call in progress: who, its state and time, answer and decline for a
/// call coming in, then mute, volume and hang up. Always dark, whatever
/// the theme: a call is a room of its own.
class CallPage extends StatefulWidget {
  const CallPage({super.key, required this.model, this.onClose});
  final ChatModel model;

  /// How the screen closes; by going back unless given.
  final VoidCallback? onClose;

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
      if (mounted) _closeNow();
    });
  }

  void _closeNow() {
    final close = widget.onClose;
    close != null ? close() : Navigator.of(context).maybePop();
  }

  @override
  Widget build(BuildContext context) => Theme(
    data: inkTheme(Brightness.dark),
    child: Builder(builder: _build),
  );

  Widget _build(BuildContext context) => ListenableBuilder(
    listenable: widget.model,
    builder: (context, _) {
      final k = Palette.of(context);
      final talk = widget.model.talk;
      final call = talk.call;
      if (call == null) {
        _closeSoon();
        return Scaffold(
          backgroundColor: k.paper,
          body: Center(
            child: Text('No call', style: display(22, color: k.muted)),
          ),
        );
      }
      final name = talk.nameOf(call.peer);
      final tint = tintOf(call.peer, name);
      final state = call.state;
      final ringing = state == 'ringing' && !call.mine;
      final waiting = state == 'offered' || state == 'ringing';
      final d = talk.connectedFor;
      final remoteVideo = state == 'connected' && talk.remoteVideoShown ? talk.remoteVideo() : null;
      final localVideo = call.live ? talk.localVideo() : null;
      // What the other side's camera is doing, when there is no picture.
      final tile = !call.webrtc || state != 'connected' ? null : remoteCameraTile[call.remoteCamera];
      final status = switch (state) {
        'offered' => 'Calling…',
        'ringing' when call.video => 'Video call',
        'ringing' => 'Calling you',
        'connected' => '${d.inMinutes}:${(d.inSeconds % 60).toString().padLeft(2, '0')}',
        _ => callOutcome(state, transport: call.transport),
      };
      if (!call.live) _closeSoon();
      return Scaffold(
        backgroundColor: k.paper,
        appBar: AppBar(
          backgroundColor: Colors.transparent,
          shape: const Border(),
          automaticallyImplyLeading: !call.live,
          title: Text('Tailnet call', style: mono(12, color: k.muted).copyWith(letterSpacing: 1.6)),
          centerTitle: true,
        ),
        extendBodyBehindAppBar: true,
        body: DecoratedBox(
          decoration: BoxDecoration(
            gradient: RadialGradient(
              center: const Alignment(0, -0.35),
              radius: 1.1,
              colors: [Color.alphaBlend(tint.withValues(alpha: 0.28), k.paper), k.paper],
            ),
          ),
          child: Grain(
            child: Stack(
              fit: StackFit.expand,
              children: [
                // The other side's picture, full size, under everything.
                if (remoteVideo != null) Positioned.fill(child: remoteVideo),
                SafeArea(
                  child: Padding(
                    padding: const EdgeInsets.fromLTRB(28, 24, 28, 28),
                    child: _content(context, call, name, tint, status, waiting, ringing, remoteVideo != null, tile),
                  ),
                ),
                // Ours, small, in a corner.
                if (localVideo != null)
                  Positioned(
                    top: MediaQuery.paddingOf(context).top + 64,
                    right: 16,
                    width: 108,
                    height: 144,
                    child: ClipRRect(
                      borderRadius: BorderRadius.circular(14),
                      child: DecoratedBox(
                        decoration: BoxDecoration(
                          color: Colors.black,
                          border: Border.all(color: k.line),
                        ),
                        child: localVideo,
                      ),
                    ),
                  ),
              ],
            ),
          ),
        ),
      );
    },
  );

  Widget _content(
    BuildContext context,
    Call call,
    String name,
    Color tint,
    String status,
    bool waiting,
    bool ringing,
    bool video,
    String? tile,
  ) {
    final k = Palette.of(context);
    final talk = widget.model.talk;
    final state = call.state;
    final cameras = talk.cameras;
    final noCamera = cameras != null && cameras.isEmpty;
    return Column(
      children: [
        const Spacer(),
        if (!video) ...[
          _Ripples(
            active: waiting,
            color: tint,
            child: Monogram(name: name, keyHex: call.peer, size: 112),
          ),
          const SizedBox(height: 28),
        ],
        Text(
          name,
          style: display(36, color: k.ink),
          textAlign: TextAlign.center,
        ),
        const SizedBox(height: 10),
        AnimatedSwitcher(
          duration: const Duration(milliseconds: 250),
          child: Text(
            status,
            key: const Key('call-status'),
            textAlign: TextAlign.center,
            style: mono(state == 'connected' ? 20 : 14, color: state == 'connected' ? k.ink : k.muted),
          ),
        ),
        if (tile != null)
          Padding(
            padding: const EdgeInsets.only(top: 14),
            child: Container(
              key: const Key('call-remote-tile'),
              padding: const EdgeInsets.symmetric(horizontal: 14, vertical: 8),
              decoration: BoxDecoration(
                color: k.card,
                borderRadius: BorderRadius.circular(20),
                border: Border.all(color: k.line),
              ),
              child: Row(
                mainAxisSize: MainAxisSize.min,
                children: [
                  Icon(Icons.videocam_off_rounded, size: 16, color: k.muted),
                  const SizedBox(width: 8),
                  Text(tile, style: mono(12, color: k.muted)),
                ],
              ),
            ),
          ),
        for (final problem in [?talk.audioProblem, ?talk.cameraProblem])
          Padding(
            padding: const EdgeInsets.only(top: 10),
            child: Text(
              problem,
              style: TextStyle(color: k.danger),
              textAlign: TextAlign.center,
            ),
          ),
        if (state == 'connected' && talk.echoCancellation != null)
          Padding(
            padding: const EdgeInsets.only(top: 10),
            child: Text('Echo cancellation: ${talk.echoCancellation}', style: mono(11, color: k.muted)),
          ),
        const Spacer(),
        if (state == 'connected')
          Padding(
            padding: const EdgeInsets.only(bottom: 18),
            child: Row(
              children: [
                Icon(Icons.volume_down_rounded, color: k.muted),
                Expanded(
                  child: Slider(
                    key: const Key('call-volume'),
                    value: talk.volume,
                    max: 2,
                    onChanged: (v) => talk.volume = v,
                  ),
                ),
                Icon(Icons.volume_up_rounded, color: k.muted),
              ],
            ),
          ),
        Row(
          mainAxisAlignment: MainAxisAlignment.spaceEvenly,
          children: [
            if (ringing) ...[
              _RoundButton(
                key: const Key('call-decline'),
                label: 'Decline',
                icon: Icons.call_end_rounded,
                color: const Color(0xFFD8452E),
                onPressed: talk.decline,
              ),
              // Plain Answer keeps the camera off, even on a
              // video call: it is never on without a tap.
              _RoundButton(
                key: const Key('call-answer'),
                label: 'Answer',
                icon: Icons.call_rounded,
                color: k.moss,
                onPressed: talk.answer,
              ),
              if (call.video)
                _RoundButton(
                  key: const Key('call-answer-video'),
                  label: 'Answer with video',
                  icon: Icons.videocam_rounded,
                  color: k.moss,
                  onPressed: () => talk.answer(null, true),
                ),
            ] else if (call.live) ...[
              _RoundButton(
                key: const Key('call-mute'),
                label: talk.muted ? 'Unmute' : 'Mute',
                tooltip: talk.muted ? 'Unmute' : 'Mute',
                icon: talk.muted ? Icons.mic_off_rounded : Icons.mic_rounded,
                color: talk.muted ? k.ink : k.card,
                foreground: talk.muted ? k.paper : k.ink,
                onPressed: state == 'connected' ? () => talk.muted = !talk.muted : null,
              ),
              if (talk.cameraSwitch)
                _RoundButton(
                  key: const Key('call-camera'),
                  label: noCamera ? 'No camera' : 'Camera',
                  tooltip: noCamera
                      ? 'No camera'
                      : (talk.cameraSwitchOn ? 'Turn the camera off' : 'Turn the camera on'),
                  icon: talk.cameraSwitchOn ? Icons.videocam_rounded : Icons.videocam_off_rounded,
                  color: talk.cameraSwitchOn ? k.ink : k.card,
                  foreground: talk.cameraSwitchOn ? k.paper : k.ink,
                  onPressed: noCamera ? null : () => talk.setCameraOn(!talk.cameraSwitchOn),
                ),
              if (talk.canSwitchCamera && talk.cameraOn)
                _RoundButton(
                  key: const Key('call-switch-camera'),
                  label: 'Switch camera',
                  tooltip: 'Switch camera',
                  icon: Icons.cameraswitch_rounded,
                  color: k.card,
                  foreground: k.ink,
                  onPressed: talk.switchCamera,
                ),
              _RoundButton(
                key: const Key('call-hangup'),
                label: 'Hang up',
                tooltip: 'Hang up',
                icon: Icons.call_end_rounded,
                color: const Color(0xFFD8452E),
                onPressed: talk.hangUp,
              ),
            ] else
              FilledButton(key: const Key('call-close'), onPressed: _closeNow, child: const Text('Close')),
          ],
        ),
      ],
    );
  }
}

class _RoundButton extends StatelessWidget {
  const _RoundButton({
    super.key,
    required this.label,
    required this.icon,
    required this.color,
    required this.onPressed,
    this.foreground = Colors.white,
    this.tooltip,
  });
  final String label;
  final String? tooltip;
  final IconData icon;
  final Color color;
  final Color foreground;
  final VoidCallback? onPressed;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    final button = Material(
      color: onPressed == null ? color.withValues(alpha: 0.4) : color,
      shape: CircleBorder(side: BorderSide(color: k.line)),
      elevation: 0,
      child: InkWell(
        customBorder: const CircleBorder(),
        onTap: onPressed,
        child: SizedBox(width: 72, height: 72, child: Icon(icon, color: foreground, size: 30)),
      ),
    );
    return Column(
      mainAxisSize: MainAxisSize.min,
      children: [
        tooltip == null ? button : Tooltip(message: tooltip, child: button),
        const SizedBox(height: 10),
        Text(label, style: mono(11.5, color: k.muted)),
      ],
    );
  }
}

/// Rings that spread from the monogram while the call waits for an answer.
class _Ripples extends StatefulWidget {
  const _Ripples({required this.active, required this.color, required this.child});
  final bool active;
  final Color color;
  final Widget child;

  @override
  State<_Ripples> createState() => _RipplesState();
}

class _RipplesState extends State<_Ripples> with SingleTickerProviderStateMixin {
  late final _c = AnimationController(vsync: this, duration: const Duration(milliseconds: 2600));

  void _follow() {
    if (widget.active && ambientMotion(context)) {
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
  void didUpdateWidget(_Ripples old) {
    super.didUpdateWidget(old);
    _follow();
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => SizedBox(
    width: 260,
    height: 260,
    child: AnimatedBuilder(
      animation: _c,
      child: widget.child,
      builder: (context, child) => Stack(
        alignment: Alignment.center,
        children: [
          if (_c.isAnimating)
            for (var i = 0; i < 3; i++)
              Builder(
                builder: (context) {
                  final t = (_c.value + i / 3) % 1;
                  final size = 116 + 140 * Curves.easeOut.transform(t);
                  return Container(
                    width: size,
                    height: size,
                    decoration: BoxDecoration(
                      shape: BoxShape.circle,
                      border: Border.all(color: widget.color.withValues(alpha: 0.5 * (1 - t)), width: 1.5),
                    ),
                  );
                },
              ),
          child!,
        ],
      ),
    ),
  );
}
