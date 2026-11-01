/// Tailnet chat for the companion app: protocol version 1, a chat peer that
/// listens and dials, its decisions, and its local database. Pure Dart; the
/// Flutter app is its UI, audio and platform glue.
library;

export 'src/control.dart' show control, identityJson;
export 'src/identity_file.dart';
export 'src/peer.dart';
export 'src/policy.dart' hide isTyping;
export 'src/protocol.dart';
export 'src/store.dart';
