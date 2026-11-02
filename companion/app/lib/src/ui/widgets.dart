import 'dart:io' show Platform;
import 'dart:math' as math;
import 'dart:ui' show PointMode;

import 'package:flutter/material.dart';

import 'theme.dart';

/// Whether what moves for its own sake runs (an online ring breathing, a
/// call's ripples): not when the platform asks for less motion, nor under
/// `flutter test`, where an animation that never ends never settles.
bool ambientMotion(BuildContext context) =>
    !MediaQuery.disableAnimationsOf(context) && !Platform.environment.containsKey('FLUTTER_TEST');

/// A faint paper grain over the page, drawn once and cached.
class Grain extends StatelessWidget {
  const Grain({super.key, required this.child});
  final Widget child;

  @override
  Widget build(BuildContext context) => Stack(
    fit: StackFit.passthrough,
    children: [
      child,
      Positioned.fill(
        child: IgnorePointer(
          child: RepaintBoundary(child: CustomPaint(painter: _GrainPainter(Palette.of(context).grain))),
        ),
      ),
    ],
  );
}

class _GrainPainter extends CustomPainter {
  _GrainPainter(this.color);
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    final r = math.Random(7);
    final dark = Paint()..color = color.withValues(alpha: 0.045);
    final light = Paint()..color = color.withValues(alpha: 0.025);
    final n = (size.width * size.height / 70).clamp(0, 60000).toInt();
    final a = <Offset>[], b = <Offset>[];
    for (var i = 0; i < n; i++) {
      (i.isEven ? a : b).add(Offset(r.nextDouble() * size.width, r.nextDouble() * size.height));
    }
    canvas.drawPoints(
      PointMode.points,
      a,
      dark
        ..strokeWidth = 1
        ..strokeCap = StrokeCap.round,
    );
    canvas.drawPoints(PointMode.points, b, light..strokeWidth = 1.6);
  }

  @override
  bool shouldRepaint(_GrainPainter old) => old.color != color;
}

/// The tint a key (or, before it is known, a name) picks.
Color tintOf(String? key, String name) {
  final s = key ?? name;
  var h = 0;
  for (final c in s.codeUnits) {
    h = (h * 31 + c) & 0x7fffffff;
  }
  return Palette.tints[h % Palette.tints.length];
}

/// A contact's monogram: its initial on its key's tint, in a soft square,
/// with a moss dot that breathes while it is connected.
class Monogram extends StatelessWidget {
  const Monogram({super.key, required this.name, this.keyHex, this.size = 46, this.online = false, this.warn = false});
  final String name;
  final String? keyHex;
  final double size;
  final bool online;
  final bool warn;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    final tint = warn ? k.danger : tintOf(keyHex, name);
    final letter = name.isEmpty ? '?' : name.characters.first.toUpperCase();
    final dark = Theme.of(context).brightness == Brightness.dark;
    return SizedBox(
      width: size + 4,
      height: size + 4,
      child: Stack(
        clipBehavior: Clip.none,
        children: [
          Container(
            width: size,
            height: size,
            alignment: Alignment.center,
            decoration: BoxDecoration(
              color: Color.alphaBlend(tint.withValues(alpha: dark ? 0.30 : 0.20), k.card),
              borderRadius: BorderRadius.circular(size * 0.34),
              border: Border.all(color: tint.withValues(alpha: 0.45)),
            ),
            child: warn
                ? Icon(Icons.priority_high_rounded, color: k.danger, size: size * 0.5)
                : Text(letter, style: display(size * 0.46, w: 620, color: Color.lerp(tint, k.ink, dark ? 0.15 : 0.45))),
          ),
          if (online) Positioned(right: 0, bottom: 0, child: OnlineDot(size: size * 0.28)),
        ],
      ),
    );
  }
}

class OnlineDot extends StatefulWidget {
  const OnlineDot({super.key, this.size = 12});
  final double size;

  @override
  State<OnlineDot> createState() => _OnlineDotState();
}

class _OnlineDotState extends State<OnlineDot> with SingleTickerProviderStateMixin {
  late final _c = AnimationController(vsync: this, duration: const Duration(milliseconds: 2400));

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (ambientMotion(context)) {
      _c.repeat();
    } else {
      _c.stop();
    }
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    final s = widget.size;
    return SizedBox(
      width: s,
      height: s,
      child: AnimatedBuilder(
        animation: _c,
        builder: (context, _) {
          final t = Curves.easeOut.transform(_c.value);
          return Stack(
            clipBehavior: Clip.none,
            alignment: Alignment.center,
            children: [
              if (_c.isAnimating)
                Container(
                  width: s * (1 + t * 1.4),
                  height: s * (1 + t * 1.4),
                  decoration: BoxDecoration(
                    shape: BoxShape.circle,
                    color: k.moss.withValues(alpha: 0.35 * (1 - t)),
                  ),
                ),
              Container(
                width: s,
                height: s,
                decoration: BoxDecoration(
                  shape: BoxShape.circle,
                  color: k.moss,
                  border: Border.all(color: k.card, width: 2),
                ),
              ),
            ],
          );
        },
      ),
    );
  }
}

/// A list's rows come in one after another: a short rise and fade, the
/// nth a little after the one above, once.
class StaggerIn extends StatefulWidget {
  const StaggerIn({super.key, required this.index, required this.child});
  final int index;
  final Widget child;

  @override
  State<StaggerIn> createState() => _StaggerInState();
}

class _StaggerInState extends State<StaggerIn> with SingleTickerProviderStateMixin {
  static const _step = 45, _each = 320;
  late final int _i = widget.index.clamp(0, 9);
  late final _c = AnimationController(
    vsync: this,
    duration: Duration(milliseconds: _each + _i * _step),
  )..forward();
  late final _curve = CurvedAnimation(
    parent: _c,
    curve: Interval(_i * _step / (_each + _i * _step), 1, curve: Curves.easeOutCubic),
  );

  @override
  void dispose() {
    _curve.dispose();
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) => AnimatedBuilder(
    animation: _curve,
    child: widget.child,
    builder: (context, child) => Opacity(
      opacity: _curve.value,
      child: Transform.translate(offset: Offset(0, 10 * (1 - _curve.value)), child: child),
    ),
  );
}

/// A small caps label over a section, with a hairline to its right.
class SectionLabel extends StatelessWidget {
  const SectionLabel(this.text, {super.key, this.trailing});
  final String text;
  final Widget? trailing;

  @override
  Widget build(BuildContext context) {
    final k = Palette.of(context);
    return Padding(
      padding: const EdgeInsets.fromLTRB(20, 18, 12, 6),
      child: Row(
        children: [
          Text(text.toUpperCase(), style: mono(11, color: k.muted, w: 560).copyWith(letterSpacing: 1.6)),
          const SizedBox(width: 10),
          Expanded(child: Container(height: 1, color: k.line)),
          ?trailing,
        ],
      ),
    );
  }
}

/// Three dots that rise in turn: the other side is typing.
class TypingDots extends StatefulWidget {
  const TypingDots({super.key, this.color});
  final Color? color;

  @override
  State<TypingDots> createState() => _TypingDotsState();
}

class _TypingDotsState extends State<TypingDots> with SingleTickerProviderStateMixin {
  late final _c = AnimationController(vsync: this, duration: const Duration(milliseconds: 1100));

  @override
  void didChangeDependencies() {
    super.didChangeDependencies();
    if (ambientMotion(context)) _c.repeat();
  }

  @override
  void dispose() {
    _c.dispose();
    super.dispose();
  }

  @override
  Widget build(BuildContext context) {
    final color = widget.color ?? Palette.of(context).signal;
    return AnimatedBuilder(
      animation: _c,
      builder: (context, _) => Row(
        mainAxisSize: MainAxisSize.min,
        children: [
          for (var i = 0; i < 3; i++)
            Padding(
              padding: const EdgeInsets.symmetric(horizontal: 1.5),
              child: Transform.translate(
                offset: Offset(0, -3 * math.max(0, math.sin((_c.value - i * 0.18) * 2 * math.pi))),
                child: Container(
                  width: 5,
                  height: 5,
                  decoration: BoxDecoration(color: color, shape: BoxShape.circle),
                ),
              ),
            ),
        ],
      ),
    );
  }
}

/// A voice message's bars: a shape of its own (from its id), filled as far
/// as it has played.
class Waveform extends StatelessWidget {
  const Waveform({super.key, required this.seed, required this.progress, required this.color, this.width = 132});
  final String seed;
  final double progress;
  final Color color;
  final double width;

  @override
  Widget build(BuildContext context) => SizedBox(
    width: width,
    height: 28,
    child: TweenAnimationBuilder<double>(
      tween: Tween(end: progress.clamp(0, 1)),
      duration: const Duration(milliseconds: 220),
      builder: (context, p, _) => CustomPaint(painter: _WavePainter(seed, p, color)),
    ),
  );
}

class _WavePainter extends CustomPainter {
  _WavePainter(this.seed, this.progress, this.color);
  final String seed;
  final double progress;
  final Color color;

  @override
  void paint(Canvas canvas, Size size) {
    var h = 17;
    for (final c in seed.codeUnits) {
      h = (h * 131 + c) & 0x7fffffff;
    }
    final r = math.Random(h);
    const bar = 3.0, gap = 2.4;
    final n = (size.width / (bar + gap)).floor();
    final on = Paint()..color = color;
    final off = Paint()..color = color.withValues(alpha: 0.32);
    for (var i = 0; i < n; i++) {
      // A rounded envelope, so it reads as speech and not as noise.
      final env = 0.35 + 0.65 * math.sin(math.pi * (i + 0.5) / n);
      final v = (0.25 + 0.75 * r.nextDouble()) * env;
      final bh = math.max(3.0, v * size.height);
      final x = i * (bar + gap);
      canvas.drawRRect(
        RRect.fromRectAndRadius(Rect.fromLTWH(x, (size.height - bh) / 2, bar, bh), const Radius.circular(2)),
        (i + 0.5) / n <= progress ? on : off,
      );
    }
  }

  @override
  bool shouldRepaint(_WavePainter old) => old.progress != progress || old.color != color || old.seed != seed;
}

/// A message pops into place from the corner it hangs from.
class PopIn extends StatelessWidget {
  const PopIn({super.key, required this.alignment, required this.child});
  final Alignment alignment;
  final Widget child;

  @override
  Widget build(BuildContext context) => TweenAnimationBuilder<double>(
    tween: Tween(begin: 0, end: 1),
    duration: const Duration(milliseconds: 360),
    curve: Curves.easeOutBack,
    child: child,
    builder: (context, t, child) => Opacity(
      opacity: t.clamp(0, 1),
      child: Transform.scale(scale: 0.86 + 0.14 * t, alignment: alignment, child: child),
    ),
  );
}

/// The fingerprint, in groups, in mono: what a person reads aloud to
/// compare.
class Fingerprint extends StatelessWidget {
  const Fingerprint(this.text, {super.key, this.size = 13, this.color});
  final String text;
  final double size;
  final Color? color;

  @override
  Widget build(BuildContext context) =>
      SelectableText(text, style: mono(size, color: color ?? Palette.of(context).ink).copyWith(height: 1.5));
}
