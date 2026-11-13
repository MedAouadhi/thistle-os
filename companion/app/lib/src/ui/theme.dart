import 'package:flutter/material.dart';

/// Palette on paper, as the T-Deck's e-paper is: warm paper, ink, hairline
/// rules, and one signal colour (vermilion) for what wants attention: send,
/// unread, calls. Moss is "online". The fonts are bundled (assets/fonts,
/// SIL OFL): Fraunces for names and titles, Instrument Sans for text,
/// JetBrains Mono for what a person compares: fingerprints, addresses,
/// times.
@immutable
class Palette extends ThemeExtension<Palette> {
  const Palette({
    required this.paper,
    required this.card,
    required this.ink,
    required this.muted,
    required this.line,
    required this.signal,
    required this.onSignal,
    required this.moss,
    required this.mine,
    required this.onMine,
    required this.danger,
    required this.grain,
  });

  final Color paper;
  final Color card;
  final Color ink;
  final Color muted;
  final Color line;
  final Color signal;
  final Color onSignal;
  final Color moss;

  /// My bubbles are the ink, theirs the paper; swapped in the dark.
  final Color mine;
  final Color onMine;
  final Color danger;
  final Color grain;

  static const light = Palette(
    paper: Color(0xFFF1ECE2),
    card: Color(0xFFFAF7F1),
    ink: Color(0xFF1B1A17),
    muted: Color(0xFF6E685D),
    line: Color(0xFFDCD5C7),
    signal: Color(0xFFD5461B),
    onSignal: Color(0xFFFFF6EE),
    moss: Color(0xFF4C7A49),
    mine: Color(0xFF23211D),
    onMine: Color(0xFFF4EFE6),
    danger: Color(0xFFA3301A),
    grain: Color(0xFF3A2F1E),
  );

  static const dark = Palette(
    paper: Color(0xFF12110F),
    card: Color(0xFF1C1A17),
    ink: Color(0xFFEFE9DE),
    muted: Color(0xFFA1998B),
    line: Color(0xFF2F2C27),
    signal: Color(0xFFEF6638),
    onSignal: Color(0xFF1A0D07),
    moss: Color(0xFF85B37D),
    mine: Color(0xFFE9E2D5),
    onMine: Color(0xFF1B1A17),
    danger: Color(0xFFF08A70),
    grain: Color(0xFFFFF2DC),
  );

  /// Earthy tints for a contact's monogram, picked by its key, so a
  /// familiar colour is a small check that the key is the one you know.
  static const tints = [
    Color(0xFFC08A3A), // ochre
    Color(0xFF6F9070), // sage
    Color(0xFFB2603F), // clay
    Color(0xFF5A7089), // slate
    Color(0xFF8C667D), // heather
    Color(0xFF85843F), // olive
    Color(0xFF3F7F86), // teal
  ];

  static Palette of(BuildContext context) => Theme.of(context).extension<Palette>()!;

  @override
  Palette copyWith() => this;

  @override
  Palette lerp(Palette? other, double t) {
    if (other == null) return this;
    Color l(Color a, Color b) => Color.lerp(a, b, t)!;
    return Palette(
      paper: l(paper, other.paper),
      card: l(card, other.card),
      ink: l(ink, other.ink),
      muted: l(muted, other.muted),
      line: l(line, other.line),
      signal: l(signal, other.signal),
      onSignal: l(onSignal, other.onSignal),
      moss: l(moss, other.moss),
      mine: l(mine, other.mine),
      onMine: l(onMine, other.onMine),
      danger: l(danger, other.danger),
      grain: l(grain, other.grain),
    );
  }
}

const displayFamily = 'Fraunces';
const textFamily = 'InstrumentSans';
const monoFamily = 'JetBrainsMono';

/// A variable font's weight: Flutter does not drive the axis from
/// `fontWeight`, it would embolden the regular instead.
List<FontVariation> weight(double w) => [FontVariation('wght', w)];

/// Fraunces, soft and a little wonky at display sizes.
TextStyle display(double size, {double w = 560, Color? color, double height = 1.1}) => TextStyle(
  fontFamily: displayFamily,
  fontSize: size,
  height: height,
  color: color,
  letterSpacing: size > 24 ? -0.6 : -0.2,
  fontVariations: [FontVariation('wght', w), const FontVariation('SOFT', 100), const FontVariation('opsz', 48)],
);

TextStyle mono(double size, {Color? color, double w = 420}) =>
    TextStyle(fontFamily: monoFamily, fontSize: size, color: color, fontVariations: weight(w), letterSpacing: 0.2);

ThemeData inkTheme(Brightness brightness) {
  final k = brightness == Brightness.dark ? Palette.dark : Palette.light;
  final scheme = ColorScheme(
    brightness: brightness,
    primary: k.ink,
    onPrimary: k.paper,
    secondary: k.signal,
    onSecondary: k.onSignal,
    tertiary: k.moss,
    onTertiary: k.paper,
    error: k.danger,
    onError: k.paper,
    surface: k.paper,
    onSurface: k.ink,
    onSurfaceVariant: k.muted,
    outline: k.line,
    outlineVariant: k.line,
    surfaceContainerLowest: k.card,
    surfaceContainerLow: k.card,
    surfaceContainer: k.card,
    surfaceContainerHigh: Color.alphaBlend(k.ink.withValues(alpha: 0.04), k.card),
    surfaceContainerHighest: Color.alphaBlend(k.ink.withValues(alpha: 0.07), k.card),
    primaryContainer: k.mine,
    onPrimaryContainer: k.onMine,
    errorContainer: Color.alphaBlend(k.danger.withValues(alpha: 0.12), k.card),
    onErrorContainer: k.danger,
    tertiaryContainer: Color.alphaBlend(k.signal.withValues(alpha: 0.10), k.card),
    onTertiaryContainer: k.ink,
    inverseSurface: k.ink,
    onInverseSurface: k.paper,
    shadow: Colors.black,
  );
  TextStyle t(double size, double w, {Color? color, double? height}) => TextStyle(
    fontFamily: textFamily,
    fontSize: size,
    height: height,
    color: color ?? k.ink,
    fontVariations: weight(w),
  );
  final base = ThemeData(brightness: brightness, colorScheme: scheme, useMaterial3: true);
  final shape = RoundedRectangleBorder(borderRadius: BorderRadius.circular(14));
  return base.copyWith(
    scaffoldBackgroundColor: k.paper,
    extensions: [k],
    splashFactory: InkSparkle.splashFactory,
    textTheme: base.textTheme.copyWith(
      displaySmall: display(34, color: k.ink),
      headlineMedium: display(28, color: k.ink),
      headlineSmall: display(24, color: k.ink),
      titleLarge: display(21, color: k.ink),
      titleMedium: t(16, 600),
      titleSmall: t(14, 600),
      bodyLarge: t(16, 420, height: 1.4),
      bodyMedium: t(14.5, 420, height: 1.4),
      bodySmall: t(12.5, 420, color: k.muted, height: 1.35),
      labelLarge: t(14, 600),
      labelMedium: t(12.5, 560),
      labelSmall: t(11, 560, color: k.muted),
    ),
    appBarTheme: AppBarTheme(
      backgroundColor: k.paper.withValues(alpha: 0.86),
      surfaceTintColor: Colors.transparent,
      foregroundColor: k.ink,
      elevation: 0,
      scrolledUnderElevation: 0,
      centerTitle: false,
      titleTextStyle: display(21, color: k.ink),
      shape: Border(bottom: BorderSide(color: k.line)),
    ),
    dividerTheme: DividerThemeData(color: k.line, thickness: 1, space: 1),
    cardTheme: CardThemeData(
      color: k.card,
      surfaceTintColor: Colors.transparent,
      elevation: 0,
      margin: const EdgeInsets.symmetric(vertical: 6),
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(18),
        side: BorderSide(color: k.line),
      ),
    ),
    listTileTheme: ListTileThemeData(
      iconColor: k.muted,
      titleTextStyle: t(15.5, 560),
      subtitleTextStyle: t(13, 420, color: k.muted),
      contentPadding: const EdgeInsets.symmetric(horizontal: 18, vertical: 2),
    ),
    filledButtonTheme: FilledButtonThemeData(
      style: FilledButton.styleFrom(
        backgroundColor: k.ink,
        foregroundColor: k.paper,
        disabledBackgroundColor: k.line,
        shape: const StadiumBorder(),
        padding: const EdgeInsets.symmetric(horizontal: 20, vertical: 14),
        textStyle: t(14, 600),
      ),
    ),
    outlinedButtonTheme: OutlinedButtonThemeData(
      style: OutlinedButton.styleFrom(
        foregroundColor: k.ink,
        side: BorderSide(color: k.line, width: 1.2),
        shape: const StadiumBorder(),
        padding: const EdgeInsets.symmetric(horizontal: 18, vertical: 14),
        textStyle: t(14, 600),
      ),
    ),
    textButtonTheme: TextButtonThemeData(
      style: TextButton.styleFrom(foregroundColor: k.signal, shape: const StadiumBorder(), textStyle: t(14, 620)),
    ),
    iconButtonTheme: IconButtonThemeData(style: IconButton.styleFrom(foregroundColor: k.ink)),
    floatingActionButtonTheme: FloatingActionButtonThemeData(
      backgroundColor: k.ink,
      foregroundColor: k.paper,
      elevation: 2,
      focusElevation: 2,
      hoverElevation: 3,
      highlightElevation: 3,
      shape: RoundedRectangleBorder(borderRadius: BorderRadius.circular(18)),
      extendedTextStyle: t(14.5, 600),
    ),
    inputDecorationTheme: InputDecorationTheme(
      filled: true,
      fillColor: k.card,
      hintStyle: t(15, 420, color: k.muted),
      labelStyle: t(14, 500, color: k.muted),
      contentPadding: const EdgeInsets.symmetric(horizontal: 18, vertical: 14),
      border: OutlineInputBorder(
        borderRadius: BorderRadius.circular(16),
        borderSide: BorderSide(color: k.line),
      ),
      enabledBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(16),
        borderSide: BorderSide(color: k.line),
      ),
      focusedBorder: OutlineInputBorder(
        borderRadius: BorderRadius.circular(16),
        borderSide: BorderSide(color: k.ink, width: 1.4),
      ),
    ),
    dialogTheme: DialogThemeData(
      backgroundColor: k.card,
      surfaceTintColor: Colors.transparent,
      shape: RoundedRectangleBorder(
        borderRadius: BorderRadius.circular(24),
        side: BorderSide(color: k.line),
      ),
      titleTextStyle: display(22, color: k.ink),
      contentTextStyle: t(14.5, 420),
    ),
    bottomSheetTheme: BottomSheetThemeData(
      backgroundColor: k.card,
      surfaceTintColor: Colors.transparent,
      showDragHandle: true,
      dragHandleColor: k.line,
      shape: const RoundedRectangleBorder(borderRadius: BorderRadius.vertical(top: Radius.circular(26))),
    ),
    snackBarTheme: SnackBarThemeData(
      behavior: SnackBarBehavior.floating,
      backgroundColor: k.ink,
      contentTextStyle: t(14, 500, color: k.paper),
      shape: shape,
    ),
    switchTheme: SwitchThemeData(
      thumbColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? k.paper : k.muted),
      trackColor: WidgetStateProperty.resolveWith((s) => s.contains(WidgetState.selected) ? k.moss : k.line),
      trackOutlineColor: WidgetStateProperty.all(Colors.transparent),
    ),
    sliderTheme: SliderThemeData(
      activeTrackColor: k.onMine,
      inactiveTrackColor: k.onMine.withValues(alpha: 0.2),
      thumbColor: k.onMine,
      overlayColor: k.onMine.withValues(alpha: 0.1),
    ),
    progressIndicatorTheme: ProgressIndicatorThemeData(color: k.signal),
    badgeTheme: BadgeThemeData(backgroundColor: k.signal, textColor: k.onSignal),
    tooltipTheme: TooltipThemeData(
      decoration: BoxDecoration(color: k.ink, borderRadius: BorderRadius.circular(8)),
      textStyle: t(12, 520, color: k.paper),
    ),
    pageTransitionsTheme: const PageTransitionsTheme(
      builders: {
        TargetPlatform.android: FadeForwardsPageTransitionsBuilder(),
        TargetPlatform.linux: FadeForwardsPageTransitionsBuilder(),
        TargetPlatform.macOS: FadeForwardsPageTransitionsBuilder(),
      },
    ),
  );
}
