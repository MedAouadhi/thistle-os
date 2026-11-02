/// The app's command line (`thistle_chat --profile second --port 4811`).
library;

import 'package:tailnet_chat_core/tailnet_chat_core.dart' show defaultPort;

class AppOptions {
  const AppOptions({
    this.profile = 'default',
    this.listenHost,
    this.port = defaultPort,
    this.discoveryPort = defaultPort,
    this.loopback = false,
    this.name,
    this.verbose = false,
    this.mic,
    this.speakerDir,
  });

  /// A second identity, database and Secret Service item on one machine.
  final String profile;

  /// Listen here instead of on the tailnet address.
  final String? listenHost;
  final int port;

  /// The port probed on other machines and dialled for a name typed in.
  final int discoveryPort;

  /// Admit loopback connections as well as the tailnet's.
  final bool loopback;

  /// The name announced in hello; the tailnet hostname by default.
  final String? name;

  /// Log the core's events to stderr.
  final bool verbose;

  /// A 16 kHz mono WAV file to use as the microphone, looped.
  final String? mic;

  /// A folder for what the speaker plays, a WAV file each.
  final String? speakerDir;

  static const usage = '''
thistle_chat [--profile NAME] [--port N] [--listen HOST] [--loopback]
             [--discovery-port N] [--name NAME] [--verbose]
             [--mic FILE.wav] [--speaker DIR]''';

  static AppOptions parse(List<String> args) {
    var o = const AppOptions();
    String value(int i) => i < args.length ? args[i] : throw FormatException('${args[i - 1]} needs a value');
    int port(String v) {
      final n = int.tryParse(v);
      if (n == null || n < 0 || n > 65535) throw FormatException('$v: not a port');
      return n;
    }

    for (var i = 0; i < args.length; i++) {
      final a = args[i];
      o = switch (a) {
        '--profile' => o._with(profile: value(++i)),
        '--listen' => o._with(listenHost: value(++i)),
        '--port' => o._with(port: port(value(++i))),
        '--discovery-port' => o._with(discoveryPort: port(value(++i))),
        '--loopback' => o._with(loopback: true),
        '--name' => o._with(name: value(++i)),
        '--verbose' => o._with(verbose: true),
        '--mic' => o._with(mic: value(++i)),
        '--speaker' => o._with(speakerDir: value(++i)),
        _ => throw FormatException('unknown option $a\n$usage'),
      };
    }
    if (!RegExp(r'^[A-Za-z0-9_-]{1,32}$').hasMatch(o.profile)) {
      throw FormatException('${o.profile}: a profile is letters, digits, - and _');
    }
    return o;
  }

  AppOptions _with({
    String? profile,
    String? listenHost,
    int? port,
    int? discoveryPort,
    bool? loopback,
    String? name,
    bool? verbose,
    String? mic,
    String? speakerDir,
  }) => AppOptions(
    profile: profile ?? this.profile,
    listenHost: listenHost ?? this.listenHost,
    port: port ?? this.port,
    discoveryPort: discoveryPort ?? this.discoveryPort,
    loopback: loopback ?? this.loopback,
    name: name ?? this.name,
    verbose: verbose ?? this.verbose,
    mic: mic ?? this.mic,
    speakerDir: speakerDir ?? this.speakerDir,
  );
}
