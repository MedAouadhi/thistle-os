/// Tailnet chat, protocol version 1: the parts with no I/O.
///
/// docs/tailnet-chat/protocol.md is the authority. Everything here is pure:
/// the identity key, the handshake's transcript, the fingerprint, the binary
/// frame header, building and checking JSON frames, and admission.
library;

import 'dart:convert';
import 'dart:io' show InternetAddress, InternetAddressType;
import 'dart:math';
import 'dart:typed_data';

import 'package:crypto/crypto.dart' show sha256;
import 'package:ed25519_edwards/ed25519_edwards.dart' as ed;

const int protocolVersion = 1;
const int defaultPort = 4810;
const String wsPath = '/tailnet-chat/1';

const List<String> capsV1 = ['text', 'receipts', 'typing', 'reply', 'react', 'voice-message', 'call:ws-opus', 'peers'];

/// Calls over `ws-opus`, and over WebRTC between video-capable chat peers
/// (protocol section 4.11).
const String capWsOpus = 'call:ws-opus';
const String capWebrtc = 'call:webrtc';

/// The transports a `call_offer` lists and a `call_answer` picks.
const String transportWsOpus = 'ws-opus';
const String transportWebrtc = 'webrtc';

/// What `call_camera` says of one side's camera (protocol section 4.11):
/// `paused` while its app is out of the foreground, `none` with no camera,
/// no permission, or one that gives no picture.
enum CameraState {
  on,
  off,
  paused,
  none;

  String get wire => name;

  /// The state a `call_camera` names, or null for another word.
  static CameraState? fromWire(Object? v) => values.where((c) => c.wire == v).firstOrNull;
}

/// A WebRTC call's media connection, as the app's engine reports it; `new`
/// until it first says.
enum MediaState {
  fresh('new'),
  connected('connected'),
  disconnected('disconnected'),
  failed('failed');

  const MediaState(this.wire);
  final String wire;

  static MediaState? fromWire(Object? v) => values.where((m) => m.wire == v).firstOrNull;
}

/// What one `call_rtc` carries: exactly one of an SDP or an ICE candidate,
/// opaque to the chat peer but for the candidates' addresses.
sealed class RtcPayload {
  const RtcPayload();

  /// The frame's member for it: `{"sdp": ...}` or `{"candidate": ...}`.
  Map<String, Object?> toJson();

  /// The payload of a checked `call_rtc` frame.
  static RtcPayload fromFrame(Map<String, dynamic> frame) {
    final sdp = frame['sdp'] as Map?, candidate = frame['candidate'] as Map?;
    if (sdp != null) return RtcSdp(sdp['type'] as String, sdp['sdp'] as String);
    return RtcCandidate(
      candidate!['candidate'] as String,
      sdpMid: candidate['sdpMid'] as String?,
      sdpMLineIndex: candidate['sdpMLineIndex'] as int?,
    );
  }
}

/// An SDP offer or answer.
class RtcSdp extends RtcPayload {
  const RtcSdp(this.type, this.sdp);

  /// `offer` or `answer`.
  final String type;
  final String sdp;

  @override
  Map<String, Object?> toJson() => {
    'sdp': {'type': type, 'sdp': sdp},
  };
}

/// One trickled ICE candidate: its line, `candidate:...`.
class RtcCandidate extends RtcPayload {
  const RtcCandidate(this.candidate, {this.sdpMid, this.sdpMLineIndex});
  final String candidate;
  final String? sdpMid;
  final int? sdpMLineIndex;

  @override
  Map<String, Object?> toJson() => {
    'candidate': {'candidate': candidate, 'sdpMid': ?sdpMid, 'sdpMLineIndex': ?sdpMLineIndex},
  };
}

const int kindVoice = 0x01;
const int kindCallAudio = 0x02;
const int maxChunk = 4096;
const int maxTextBytes = 4096;
const int maxJsonFrame = 16384;
const int maxVoiceMs = 120000;
const int maxVoiceBytes = 512 * 1024;
const int maxNameBytes = 64;
const int maxPeers = 64;
const int maxPeerAddrs = 2;

const int closeHandshake = 4000;
const int closeDuplicate = 4001;
const int closeKeyChanged = 4002;
const int closeIdle = 1000;
const int closeGoingAway = 1001;

const Duration handshakeTimeout = Duration(seconds: 10);
const Duration typingLapse = Duration(seconds: 6);
const Duration pingInterval = Duration(seconds: 20);
const Duration idleTimeout = Duration(minutes: 2);

final Random _random = Random.secure();

Uint8List randomBytes(int n) => Uint8List.fromList(List.generate(n, (_) => _random.nextInt(256)));

/// A fresh 128-bit id: a message, a reaction, a call.
String newId() => toHex(randomBytes(16));

int nowMs() => DateTime.now().millisecondsSinceEpoch;

String toHex(List<int> bytes) => bytes.map((b) => b.toRadixString(16).padLeft(2, '0')).join();

Uint8List fromHex(String hex) {
  if (hex.length.isOdd) throw FormatException('odd hex', hex);
  return Uint8List.fromList([for (var i = 0; i < hex.length; i += 2) int.parse(hex.substring(i, i + 2), radix: 16)]);
}

bool isLowerHex(Object? v, int length) =>
    v is String && v.length == length && v.codeUnits.every((c) => (c >= 48 && c <= 57) || (c >= 97 && c <= 102));

// -- identity -------------------------------------------------------------------

/// An Ed25519 identity key, from its 32-byte seed.
class Identity {
  Identity.fromSeed(List<int> seed)
    : seed = Uint8List.fromList(seed),
      _private = ed.newKeyFromSeed(Uint8List.fromList(seed)) {
    if (seed.length != 32) throw ArgumentError('a seed is 32 bytes');
    key = Uint8List.fromList(ed.public(_private).bytes);
  }

  factory Identity.generate() => Identity.fromSeed(randomBytes(32));

  final Uint8List seed;
  final ed.PrivateKey _private;
  late final Uint8List key;

  String get keyHex => toHex(key);
  String get fingerprint => fingerprintOf(key);

  Uint8List sign(List<int> message) => ed.sign(_private, Uint8List.fromList(message));
}

bool verifySignature(List<int> key, List<int> message, List<int> sig) {
  if (key.length != 32 || sig.length != 64) return false;
  try {
    return ed.verify(ed.PublicKey(key), Uint8List.fromList(message), Uint8List.fromList(sig));
  } catch (_) {
    return false;
  }
}

/// The key's fingerprint as one word: a file name, a log line.
String fingerprintId(List<int> key) => toHex(sha256.convert(key).bytes.sublist(0, 10));

/// The key's fingerprint as a person compares it: five groups of four.
String fingerprintOf(List<int> key) {
  final w = fingerprintId(key);
  return [for (var i = 0; i < 20; i += 4) w.substring(i, i + 4)].join(' ');
}

// -- the handshake --------------------------------------------------------------

enum Role { dialler, listener }

final Uint8List _label = Uint8List.fromList([...utf8.encode('tailnet-chat/1 auth'), 0]);

/// What `role` signs in its `auth`: 149 bytes.
Uint8List transcript(
  Role role,
  List<int> diallerNonce,
  List<int> listenerNonce,
  List<int> diallerKey,
  List<int> listenerKey,
) {
  for (final (name, v) in [
    ('dialler nonce', diallerNonce),
    ('listener nonce', listenerNonce),
    ('dialler key', diallerKey),
    ('listener key', listenerKey),
  ]) {
    if (v.length != 32) throw ArgumentError('$name must be 32 bytes');
  }
  return Uint8List.fromList([
    ..._label,
    role == Role.dialler ? 1 : 2,
    ...diallerNonce,
    ...listenerNonce,
    ...diallerKey,
    ...listenerKey,
  ]);
}

Map<String, Object?> helloFrame(
  List<int> key,
  String name,
  List<String> caps,
  List<int> nonce, {
  int version = protocolVersion,
}) => {'type': 'hello', 'version': version, 'key': toHex(key), 'name': name, 'caps': caps, 'nonce': toHex(nonce)};

Map<String, Object?> authFrame(List<int> sig) => {'type': 'auth', 'sig': toHex(sig)};

Map<String, Object?> errorFrame(String code, {String? what, String? id, String? detail}) => {
  'type': 'error',
  'code': code,
  if (what != null) 'what': what,
  if (id != null) 'id': id,
  if (detail != null) 'detail': detail,
};

/// A tailnet peer as a `peers` answer lists it (protocol section 4.10).
class SharedPeer {
  const SharedPeer(this.name, this.addrs);

  /// Its tailnet hostname
  final String name;
  final List<String> addrs;

  Map<String, Object?> toJson() => {'name': name, 'addrs': addrs};

  @override
  bool operator ==(Object other) =>
      other is SharedPeer &&
      other.name == name &&
      other.addrs.length == addrs.length &&
      Iterable<int>.generate(addrs.length).every((i) => other.addrs[i] == addrs[i]);
  @override
  int get hashCode => Object.hash(name, Object.hashAll(addrs));
  @override
  String toString() => '$name ${addrs.join(',')}';
}

Map<String, Object?> peersAskFrame() => {'type': 'peers_ask'};

Map<String, Object?> peersFrame(List<SharedPeer> peers) => {
  'type': 'peers',
  'peers': [for (final p in peers) p.toJson()],
};

/// The `peers` of a checked `peers` frame.
List<SharedPeer> peersOf(Map<String, dynamic> frame) => [
  for (final p in (frame['peers'] as List).cast<Map<String, dynamic>>())
    SharedPeer(p['name'] as String, (p['addrs'] as List).cast<String>()),
];

/// A JSON frame as the vectors write it: compact, UTF-8, not ASCII-escaped.
String encodeFrame(Map<String, Object?> frame) => jsonEncode(frame);

// -- binary frames --------------------------------------------------------------

class BinaryFrame {
  BinaryFrame(this.kind, this.stream, this.seq, this.payload);
  final int kind, stream, seq;
  final Uint8List payload;
}

Uint8List packBinary(int kind, int stream, int seq, List<int> payload) {
  if (payload.length > maxChunk) throw ArgumentError('a payload is at most 4096 bytes');
  final out = Uint8List(9 + payload.length);
  final view = ByteData.sublistView(out);
  view.setUint8(0, kind);
  view.setUint32(1, stream);
  view.setUint32(5, seq);
  out.setRange(9, out.length, payload);
  return out;
}

BinaryFrame unpackBinary(List<int> frame) {
  if (frame.length < 9) throw const FormatException('a binary frame is at least its 9-byte header');
  if (frame.length - 9 > maxChunk) throw const FormatException("a binary frame's payload is at most 4096 bytes");
  final bytes = Uint8List.fromList(frame);
  final view = ByteData.sublistView(bytes);
  return BinaryFrame(view.getUint8(0), view.getUint32(1), view.getUint32(5), bytes.sublist(9));
}

// -- admission ------------------------------------------------------------------

/// `address` as an IP address, an IPv4-mapped one as its IPv4 address; null
/// if it is not one.
InternetAddress? ipOf(String address) {
  final ip = InternetAddress.tryParse(address.split('%').first);
  if (ip == null) return null;
  final raw = ip.rawAddress;
  if (ip.type == InternetAddressType.IPv6 &&
      raw.sublist(0, 10).every((b) => b == 0) &&
      raw[10] == 0xff &&
      raw[11] == 0xff) {
    return InternetAddress.fromRawAddress(raw.sublist(12));
  }
  return ip;
}

/// Whether `address` is inside the tailnet's ranges.
bool inTailnet(String address) => admitted(address);

/// Whether a connection from `address` may reach the WebSocket upgrade.
bool admitted(String address, {bool loopback = false}) {
  final ip = ipOf(address);
  if (ip == null) return false;
  final raw = ip.rawAddress;
  if (raw.length == 4) {
    if (loopback && raw[0] == 127) return true;
    return raw[0] == 100 && (raw[1] & 0xc0) == 64; // 100.64.0.0/10
  }
  if (loopback && ip.isLoopback) return true;
  const tailnet6 = [0xfd, 0x7a, 0x11, 0x5c, 0xa1, 0xe0]; // fd7a:115c:a1e0::/48
  for (var i = 0; i < 6; i++) {
    if (raw[i] != tailnet6[i]) return false;
  }
  return true;
}

// -- checking a JSON frame -----------------------------------------------------

typedef _Ok = bool Function(Object? v);

_Ok _hex(int n) =>
    (v) => isLowerHex(v, n);
_Ok _int(int lo, int hi) =>
    (v) => v is int && v >= lo && v <= hi;
_Ok _str(int maxBytes, {bool empty = true}) =>
    (v) => v is String && (empty || v.isNotEmpty) && utf8.encode(v).length <= maxBytes;
_Ok _oneOf(List<String> values) =>
    (v) => values.contains(v);
_Ok _list(_Ok item, int lo, int hi) =>
    (v) => v is List && v.length >= lo && v.length <= hi && v.every(item);
bool _peer(Object? v) =>
    v is Map &&
    _str(maxNameBytes, empty: false)(v['name']) &&
    _list(_str(64, empty: false), 1, maxPeerAddrs)(v['addrs']);
bool _bool(Object? v) => v is bool;
bool _sdp(Object? v) =>
    v is Map && _oneOf(['offer', 'answer'])(v['type']) && _str(maxJsonFrame, empty: false)(v['sdp']);
bool _candidate(Object? v) =>
    v is Map &&
    _str(1024, empty: false)(v['candidate']) &&
    (v['sdpMid'] == null || _str(64)(v['sdpMid'])) &&
    (v['sdpMLineIndex'] == null || _int(0, 65535)(v['sdpMLineIndex']));
bool _anyString(Object? v) => v is String;

final _id = _hex(32);
final _u32 = _int(1, 0xFFFFFFFF);
final _ts = _int(0, 1 << 53);

/// type -> (required members, optional members). Others are ignored.
final Map<String, (Map<String, _Ok>, Map<String, _Ok>)> _schema = {
  'hello': (
    {
      'version': _int(0, 1 << 31),
      'key': _hex(64),
      'name': _str(maxNameBytes),
      'caps': _list(_str(64, empty: false), 0, 64),
      'nonce': _hex(64),
    },
    {},
  ),
  'auth': ({'sig': _hex(128)}, {}),
  'msg': ({'id': _id, 'ts': _ts, 'text': _anyString}, {'reply_to': _id}),
  'ack': ({'id': _id}, {}),
  'read': ({'ids': _list(_id, 1, 100)}, {}),
  'typing': ({'on': _bool}, {}),
  'react': ({'id': _id, 'ts': _ts, 'to': _id, 'emoji': _str(32)}, {}),
  'contact': (
    {
      'id': _id,
      'state': _oneOf(['accepted', 'declined']),
    },
    {},
  ),
  'voice': (
    {
      'id': _id,
      'ts': _ts,
      'ms': _int(1, maxVoiceMs),
      'bytes': _int(1, maxVoiceBytes),
      'codec': _str(32, empty: false),
      'stream': _u32,
    },
    {'reply_to': _id},
  ),
  'call_offer': ({'call': _id, 'transports': _list(_str(32, empty: false), 1, 8), 'stream': _u32}, {'video': _bool}),
  'call_answer': ({'call': _id}, {'transport': _str(32, empty: false), 'decline': _str(32, empty: false)}),
  'call_end': ({'call': _id, 'reason': _str(32, empty: false)}, {}),
  'call_rtc': ({'call': _id}, {'sdp': _sdp, 'candidate': _candidate}),
  'call_camera': (
    {
      'call': _id,
      'camera': _oneOf([for (final c in CameraState.values) c.wire]),
    },
    {},
  ),
  'peers_ask': ({}, {}),
  'peers': ({'peers': _list(_peer, 0, maxPeers)}, {}),
  'error': ({'code': _str(32, empty: false)}, {'what': _str(256), 'id': _id, 'detail': _str(512)}),
};

/// A checked frame, or the `error` frame to answer it with.
class Checked {
  const Checked.ok(Map<String, dynamic> this.frame) : error = null;
  const Checked.refused(Map<String, Object?> this.error) : frame = null;
  final Map<String, dynamic>? frame;
  final Map<String, Object?>? error;
}

String _clip(String s, int maxBytes) {
  var out = s;
  while (utf8.encode(out).length > maxBytes) {
    out = out.substring(0, out.length - 1);
  }
  return out;
}

/// Parse and check one JSON text frame (section 4 of the protocol).
Checked checkFrame(String text) {
  if (utf8.encode(text).length > maxJsonFrame) {
    return Checked.refused(errorFrame('too_large', detail: 'frame over 16384 bytes'));
  }
  Object? decoded;
  try {
    decoded = jsonDecode(text);
  } on FormatException {
    return Checked.refused(errorFrame('bad_request', detail: 'not JSON'));
  }
  if (decoded is! Map<String, dynamic> || decoded['type'] is! String) {
    return Checked.refused(errorFrame('bad_request', detail: 'not an object with a type'));
  }
  final frame = decoded;
  final kind = frame['type'] as String;
  final schema = _schema[kind];
  if (schema == null) return Checked.refused(errorFrame('unsupported', what: _clip(kind, 256)));
  final (required, optional) = schema;
  for (final MapEntry(:key, :value) in required.entries) {
    if (!frame.containsKey(key) || !value(frame[key])) {
      return Checked.refused(errorFrame('bad_request', what: kind, detail: 'field $key'));
    }
  }
  for (final MapEntry(:key, :value) in optional.entries) {
    if (frame.containsKey(key) && !value(frame[key])) {
      return Checked.refused(errorFrame('bad_request', what: kind, detail: 'field $key'));
    }
  }
  if (kind == 'msg') {
    final t = frame['text'] as String;
    if (utf8.encode(t).length > maxTextBytes) {
      return Checked.refused(errorFrame('too_large', what: 'msg', id: frame['id'] as String));
    }
    if (t.isEmpty) return Checked.refused(errorFrame('bad_request', what: 'msg', detail: 'empty text'));
  }
  if (kind == 'voice' && frame['codec'] != 'ogg-opus') {
    return Checked.refused(errorFrame('unsupported', what: 'codec:${frame['codec']}', id: frame['id'] as String));
  }
  if (kind == 'call_answer' && frame.containsKey('transport') == frame.containsKey('decline')) {
    return Checked.refused(errorFrame('bad_request', what: kind, detail: 'transport or decline'));
  }
  if (kind == 'call_rtc' && frame.containsKey('sdp') == frame.containsKey('candidate')) {
    return Checked.refused(errorFrame('bad_request', what: kind, detail: 'sdp or candidate'));
  }
  return Checked.ok(frame);
}
