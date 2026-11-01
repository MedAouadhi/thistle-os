/// Tailnet chat's decisions, with no I/O: trust, the outbox's retries, the
/// rejoin's dial list, duplicate connections, capabilities, typing,
/// reactions and the call rules (spec, "Policy apart from I/O").
library;

import 'dart:math';

import 'protocol.dart' show admitted, typingLapse;

/// A chat peer's address to dial.
class PeerAddress {
  const PeerAddress(this.host, this.port);
  final String host;
  final int port;

  @override
  bool operator ==(Object other) => other is PeerAddress && other.host == host && other.port == port;
  @override
  int get hashCode => Object.hash(host, port);
  @override
  String toString() => host.contains(':') ? '[$host]:$port' : '$host:$port';
}

/// What trust needs of a contact.
class ContactView {
  const ContactView({required this.id, this.key, this.address, this.tailnetName, this.declinedByThem = false});
  final int id;
  final String? key;
  final PeerAddress? address;
  final String? tailnetName;
  final bool declinedByThem;
}

/// Who a proven key is (protocol section 7).
sealed class Trust {
  const Trust();
}

/// Its key is pinned for `contact`.
class KnownContact extends Trust {
  const KnownContact(this.contact);
  final int contact;
}

/// Dialled at the address the user gave `contact`, which has no key yet:
/// the tailnet is trusted once, and this key is pinned.
class PinKey extends Trust {
  const PinKey(this.contact);
  final int contact;
}

/// Another key under `contact`'s name or address: refused and flagged.
class KeyMismatch extends Trust {
  const KeyMismatch(this.contact);
  final int contact;
}

/// Anyone else: what it sends is held as a contact request.
class Stranger extends Trust {
  const Stranger();
}

/// Classify the key a connection proved.
///
/// `weDialled` and `dialled`: whether this side dialled, and the address it
/// dialled. `remoteHost`: the connection's source address; it names a
/// contact only on the tailnet, as on loopback every test peer shares one.
/// `peerName`: the tailnet hostname where the platform knows it, otherwise
/// the hello's name.
Trust classify({
  required String key,
  required String? peerName,
  required bool weDialled,
  required PeerAddress? dialled,
  required String remoteHost,
  required List<ContactView> contacts,
}) {
  for (final c in contacts) {
    if (c.key == key) return KnownContact(c.id);
  }
  for (final c in contacts) {
    final bySource = !weDialled && c.address != null && c.address!.host == remoteHost && admitted(remoteHost);
    final byAddress = (weDialled && dialled != null && c.address == dialled) || bySource;
    final byName = c.tailnetName != null && c.tailnetName == peerName;
    if ((byAddress || byName) && c.key != null && c.key != key) return KeyMismatch(c.id);
    if (byAddress && weDialled && c.key == null) return PinKey(c.id);
  }
  return const Stranger();
}

// -- the outbox -----------------------------------------------------------------

const Duration firstRetry = Duration(seconds: 30);
const Duration lastRetry = Duration(minutes: 30);

/// How long to wait after `failures` failed attempts in a row: 30 s,
/// doubling, to 30 minutes. Nothing expires.
Duration retryDelay(int failures) {
  final seconds = firstRetry.inSeconds * pow(2, min(failures, 16));
  return Duration(seconds: min(seconds.toInt(), lastRetry.inSeconds));
}

/// Whether a frame of this type waits in the outbox until acked.
bool isQueued(String type) => const {'msg', 'react', 'voice', 'contact'}.contains(type);

/// Whom to dial after a rejoin (a wake, a network change) or when a peer
/// list arrives: each contact with something waiting, and each contact
/// seen online by its tailnet name, but none that declined us.
List<int> toDial(List<ContactView> contacts, {required Set<int> waiting, required Set<String> online}) => [
  for (final c in contacts)
    if (!c.declinedByThem && (waiting.contains(c.id) || (c.tailnetName != null && online.contains(c.tailnetName))))
      c.id,
];

// -- connections ----------------------------------------------------------------

/// Of two authenticated connections between the same two keys, keep the one
/// dialled by the lower key (keys as hex compare as their bytes). Dialled
/// by the same side, the newer replaces the stale one.
bool keepNewConnection({required String existingDialledBy, required String newDialledBy}) =>
    newDialledBy.compareTo(existingDialledBy) <= 0;

/// A feature is used only if both hellos announce it.
bool mayUse(String cap, List<String> mine, List<String> theirs) => mine.contains(cap) && theirs.contains(cap);

/// Whether a peer is typing, `typingLapse` after its last `typing on`.
bool isTyping(DateTime? lastOn, DateTime now) => lastOn != null && now.difference(lastOn) < typingLapse;

/// One reaction per sender per message: a later one replaces it.
bool reactionReplaces({required int? existingTs, required int newTs}) => existingTs == null || newTs >= existingTs;

// -- calls ----------------------------------------------------------------------

enum CallPolicy { answer, decline, busy, ignore }

class CallAnswer {
  const CallAnswer.pick(String this.transport) : decline = null, ringing = false;
  const CallAnswer.refuse(String this.decline) : transport = null, ringing = false;
  const CallAnswer.ring() : transport = null, decline = null, ringing = true;
  final String? transport;
  final String? decline;

  /// Neither yet: the user decides.
  final bool ringing;
}

/// How to answer a call offer: one call at a time, contacts only, a
/// transport in common.
CallAnswer answerOffer({
  required bool isContact,
  required bool busy,
  required List<String> transports,
  required List<String> caps,
  required CallPolicy policy,
}) {
  if (!isContact) return const CallAnswer.refuse('declined');
  if (busy) return const CallAnswer.refuse('busy');
  if (!transports.contains('ws-opus') || !caps.contains('call:ws-opus')) {
    return const CallAnswer.refuse('unsupported');
  }
  return switch (policy) {
    CallPolicy.answer => const CallAnswer.pick('ws-opus'),
    CallPolicy.decline => const CallAnswer.refuse('declined'),
    CallPolicy.busy => const CallAnswer.refuse('busy'),
    CallPolicy.ignore => const CallAnswer.ring(),
  };
}
