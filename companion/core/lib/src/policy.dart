/// Tailnet chat's decisions, with no I/O: trust, the outbox's retries, the
/// rejoin's dial list, duplicate connections, capabilities, typing,
/// reactions and the call rules (spec, "Policy apart from I/O").
library;

import 'dart:convert';
import 'dart:math';

import 'protocol.dart'
    show SharedPeer, admitted, capWebrtc, capWsOpus, inTailnet, ipOf, maxNameBytes, maxPeerAddrs, maxPeers, typingLapse;

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

/// The transports a call offer lists, best first (protocol 4.8): `webrtc`
/// when both hellos announce `call:webrtc`, then `ws-opus` when both
/// announce `call:ws-opus`. Empty: no call is possible.
List<String> offerTransports(List<String> mine, List<String> theirs) => [
  if (mayUse(capWebrtc, mine, theirs)) 'webrtc',
  if (mayUse(capWsOpus, mine, theirs)) 'ws-opus',
];

/// The transport an answer picks from an offer: `webrtc` whenever it is
/// offered and the callee announced it, else `ws-opus`; null if neither.
String? pickTransport(List<String> offered, List<String> caps) {
  if (offered.contains('webrtc') && caps.contains(capWebrtc)) return 'webrtc';
  if (offered.contains('ws-opus') && caps.contains(capWsOpus)) return 'ws-opus';
  return null;
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
  final pick = pickTransport(transports, caps);
  if (pick == null) return const CallAnswer.refuse('unsupported');
  return switch (policy) {
    CallPolicy.answer => CallAnswer.pick(pick),
    CallPolicy.decline => const CallAnswer.refuse('declined'),
    CallPolicy.busy => const CallAnswer.refuse('busy'),
    CallPolicy.ignore => const CallAnswer.ring(),
  };
}

/// How long a call may ring unanswered before it ends as `timeout`.
const ringTimeout = Duration(seconds: 45);

/// A connected call ends after this long without a frame (protocol 4.8).
const silenceTimeout = Duration(seconds: 10);

/// A WebRTC call whose media has not connected this long after the answer
/// ends as `timeout`.
const mediaConnectTimeout = Duration(seconds: 15);

/// A WebRTC call whose media has been disconnected this long ends as `error`.
const mediaLostTimeout = Duration(seconds: 10);

/// The reason a call ends by itself now, or null: `since` is when it
/// entered `state`, `lastFrame` when the other side's audio last came.
/// A WebRTC call carries no frames of ours: `media` is its connection's
/// state (`new`, `connected`, `disconnected`, `failed`), `mediaSince` when
/// that last changed, and `mediaEverConnected` whether it ever connected.
String? callEndDue({
  required String state,
  required DateTime since,
  required DateTime lastFrame,
  required DateTime now,
  String? transport,
  String media = 'new',
  DateTime? mediaSince,
  bool mediaEverConnected = false,
}) => switch (state) {
  'connected' when transport == 'webrtc' =>
    !mediaEverConnected
        ? (now.difference(since) > mediaConnectTimeout ? 'timeout' : null)
        : (media != 'connected' && now.difference(mediaSince ?? since) > mediaLostTimeout ? 'error' : null),
  'connected' when now.difference(lastFrame) > silenceTimeout => 'timeout',
  'offered' || 'ringing' when now.difference(since) > ringTimeout => 'timeout',
  _ => null,
};

// -- WebRTC's candidates: the tailnet's alone (protocol 4.11) ----------------------

/// Whether an ICE candidate may be sent or taken: UDP, from an address in
/// the tailnet's ranges. `candidate` is the candidate's line, with or
/// without SDP's `a=`; a host name (mDNS) is not an address.
bool candidateAdmitted(String candidate) {
  var line = candidate.trim();
  if (line.startsWith('a=')) line = line.substring(2);
  if (!line.startsWith('candidate:')) return false;
  final f = line.split(RegExp(r'\s+'));
  if (f.length < 8 || f[2].toLowerCase() != 'udp') return false;
  final ip = ipOf(f[4]);
  return ip != null && inTailnet(ip.address);
}

/// A candidate's kind, for a log: its type, its protocol, and whether its
/// address is the tailnet's (`host/udp tailnet`, `srflx/udp other`).
String candidateKind(String candidate) {
  final f = candidate.trim().replaceFirst(RegExp('^a='), '').split(RegExp(r'\s+'));
  final typ = f.indexOf('typ');
  final ip = f.length > 4 ? ipOf(f[4]) : null;
  final where = ip != null && inTailnet(ip.address) ? 'tailnet' : 'other';
  return '${typ >= 0 && typ + 1 < f.length ? f[typ + 1] : '?'}/${f.length > 2 ? f[2].toLowerCase() : '?'} $where';
}

/// An SDP with each `a=candidate` line outside the tailnet taken out, the
/// rest as it was.
String tailnetOnlySdp(String sdp) => [
  for (final m in RegExp(r'[^\n]*\n|[^\n]+$').allMatches(sdp))
    if (!m[0]!.startsWith('a=candidate:') || candidateAdmitted(m[0]!)) m[0]!,
].join();

/// A call to show as missed: someone called, and it ended without being
/// answered and without the user turning it down.
bool isMissedCall({required bool mine, required bool answered, required String state}) =>
    !mine && !answered && (state.startsWith('ended:') || state == 'declined:busy');

// -- sharing the tailnet's peer list ----------------------------------------------

String _cut(String s, int maxBytes) {
  var out = s;
  while (utf8.encode(out).length > maxBytes) {
    out = out.substring(0, out.length - 1);
  }
  return out;
}

/// What a `peers_ask` is answered with (protocol section 4.10): for a
/// contact, the tailnet peers `online` lists (the platform's online peers),
/// less the one at the asker's address, each with a name cut to 64 bytes
/// and at most two of its addresses inside the tailnet's ranges, at most 64
/// of them; for a stranger, none.
List<SharedPeer> sharedPeers(List<SharedPeer> online, {String? asker, required bool contact}) {
  if (!contact) return const [];
  final mine = asker == null ? null : ipOf(asker)?.address;
  final out = <SharedPeer>[];
  for (final p in online) {
    final ips = [for (final a in p.addrs) ipOf(a)?.address];
    if (mine != null && ips.contains(mine)) continue;
    final addrs = [
      for (final a in ips)
        if (a != null && inTailnet(a)) a,
    ].take(maxPeerAddrs).toList();
    final name = _cut(p.name, maxNameBytes);
    if (name.isNotEmpty && addrs.isNotEmpty) out.add(SharedPeer(name, addrs));
    if (out.length == maxPeers) break;
  }
  return out;
}

/// The addresses an asker may probe from a `peers` answer: those inside the
/// tailnet's ranges, each once. Nothing else of the list is taken: no key,
/// no name, no trust (protocol section 4.10).
List<String> addressesToProbe(List<SharedPeer> peers) {
  final out = <String>[];
  for (final p in peers) {
    for (final a in p.addrs) {
      final ip = ipOf(a)?.address;
      if (ip != null && inTailnet(ip) && !out.contains(ip)) out.add(ip);
    }
  }
  return out;
}
