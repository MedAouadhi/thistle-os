/// A chat peer: it listens and dials, proves its identity key, keeps its
/// contacts, requests, conversations and outbox in a [Store], and acts on
/// what other chat peers send. Decisions are [policy]'s; this is the I/O.
library;

import 'dart:async';
import 'dart:io';
import 'dart:typed_data';

import 'policy.dart' hide isTyping;
import 'policy.dart' as policy show isTyping;
import 'protocol.dart';
import 'store.dart';

/// A handshake that did not end in two proven keys, a peer that refused
/// us, or a send to a peer that declined our request.
class Refused implements Exception {
  Refused(this.why, {this.code, this.error});
  final String why;
  final int? code;
  final String? error;
  @override
  String toString() => 'Refused: $why';
}

/// Something that changed, for a UI to redraw: `connected`,
/// `disconnected`, `refused`, `request`, `message`, `delivered`, `read`,
/// `typing`, `reaction`, `contact`, `key_changed`, `call`, `error`.
class ChatEvent {
  ChatEvent(this.kind, {this.key, this.id, this.detail});
  final String kind;
  final String? key;
  final String? id;
  final String? detail;
  @override
  String toString() =>
      [kind, if (key != null) key!.substring(0, 16), if (id != null) id, if (detail != null) detail].join(' ');
}

/// What a probe heard: the hello of a chat peer at an address.
class ProbeResult {
  ProbeResult(this.address, this.key, this.name, this.fingerprint, this.caps);
  final PeerAddress address;
  final String key;
  final String name;
  final String fingerprint;
  final List<String> caps;
}

class Call {
  Call({
    required this.peer,
    required this.stream,
    required this.mine,
    required String state,
    this.transports = const [],
    this.video = false,
  }) : _state = state;
  final String peer;
  final int stream;
  final bool mine;

  /// What the offer listed, best first.
  final List<String> transports;

  /// Whether the caller's camera was on as it rang (the offer's `video`).
  final bool video;

  /// The transport the answer picked, once answered: [transportWsOpus] or
  /// [transportWebrtc].
  String? transport;

  /// The other side's camera, from its last `call_camera`; null before it
  /// says.
  CameraState? remoteCamera;

  /// A WebRTC call's media connection, as the app's engine reports it, and
  /// since when.
  MediaState media = MediaState.fresh;
  DateTime mediaSince = DateTime.now();
  bool mediaEverConnected = false;
  String _state;

  /// offered, ringing, connected, declined:<reason>, ended:<reason>.
  String get state => _state;
  set state(String s) {
    if (s == _state) return;
    _state = s;
    since = DateTime.now();
    if (s == 'connected') answered = true;
  }

  /// When it entered its state.
  DateTime since = DateTime.now();

  /// Whether it was ever connected.
  bool answered = false;
  int frames = 0;
  DateTime lastFrame = DateTime.now();
  bool get live => state == 'offered' || state == 'ringing' || state == 'connected';
  bool get webrtc => transport == transportWebrtc;

  /// Someone called, and it ended unanswered without the user declining.
  bool get missed => isMissedCall(mine: mine, answered: answered, state: state);
}

/// One packet of the other side's call audio, as it arrived.
class CallAudio {
  CallAudio(this.call, this.seq, this.packet);
  final String call;
  final int seq;
  final Uint8List packet;
}

/// The other side's SDP or one of its ICE candidates on a WebRTC call, for
/// the app's engine: opaque values, the tailnet's candidates alone.
class CallRtc {
  CallRtc(this.call, this.payload);
  final String call;
  final RtcPayload payload;
}

/// A call as the log names it: `call` and the first 8 hex digits of its id.
String callLabel(String id) => 'call ${id.substring(0, 8)}';

class _Transfer {
  _Transfer(this.frame);
  final Map<String, dynamic> frame;
  final BytesBuilder data = BytesBuilder(copy: false);
  int next = 0;
}

enum _Stage { hello, auth, open }

class _Connection {
  _Connection(this.peer, this.ws, this.role, this.remoteHost, {this.dialled});

  final ChatPeer peer;
  final WebSocket ws;
  final Role role;
  final String remoteHost;
  final PeerAddress? dialled;
  final Uint8List nonce = randomBytes(32);
  final Completer<void> ready = Completer<void>();
  final Set<String> sentIds = {};
  final Map<int, _Transfer> transfers = {};
  _Stage stage = _Stage.hello;
  Uint8List? peerKey;
  Uint8List? peerNonce;
  String? peerName;
  List<String> peerCaps = const [];
  Completer<List<SharedPeer>>? peersAsked; // a `peers_ask` sent and not yet answered
  bool closed = false;
  DateTime lastActivity = DateTime.now();
  Timer? handshakeTimer;

  String get key => peerKey == null ? '' : toHex(peerKey!);
  bool get authenticated => stage == _Stage.open;
  String get dialledBy => role == Role.dialler ? peer.identity.keyHex : key;

  void send(Map<String, Object?> frame) {
    if (closed) return;
    lastActivity = DateTime.now();
    ws.add(encodeFrame(frame));
  }

  void sendBinary(int kind, int stream, int seq, List<int> payload) {
    if (closed) return;
    lastActivity = DateTime.now();
    ws.add(packBinary(kind, stream, seq, payload));
  }

  void close(int code, [String reason = '']) {
    if (closed) return;
    closed = true;
    handshakeTimer?.cancel();
    ws.close(code, reason);
    if (!ready.isCompleted) ready.completeError(Refused('closed: $reason', code: code));
    if (peersAsked?.isCompleted == false) peersAsked!.complete(const []);
  }

  Uint8List transcriptFor(Role signer) {
    final (dn, ln, dk, lk) = role == Role.dialler
        ? (nonce, peerNonce!, peer.identity.key, peerKey!)
        : (peerNonce!, nonce, peerKey!, peer.identity.key);
    return transcript(signer, dn, ln, dk, lk);
  }
}

class ChatPeer {
  ChatPeer({
    required this.identity,
    required this.store,
    this.name = 'companion',
    this.host = '127.0.0.1',
    this.port = defaultPort,
    this.loopback = false,
    List<String> caps = capsV1,
    this.callPolicy = CallPolicy.answer,
    this.log,
  }) : _caps = List.unmodifiable(caps);

  final Identity identity;
  final Store store;
  final String name;
  final String host;
  int port;

  /// Admit loopback as well as the tailnet: for tests and the simulator.
  final bool loopback;
  List<String> _caps;

  /// What this peer's hellos announce; a change applies to connections
  /// made after it.
  List<String> get caps => _caps;
  set caps(List<String> v) => _caps = List.unmodifiable(v);
  CallPolicy callPolicy;
  final void Function(String line)? log;

  final Map<String, Call> calls = {};

  /// What each chat peer's last proven hello announced, by key, this run.
  final Map<String, List<String>> _heardCaps = {};

  /// Whether a call with `key` may carry video: its last hello announced
  /// `call:webrtc` and ours does. A contact's last hello is kept with it, so
  /// this survives a restart; anyone else is not until heard from.
  bool videoCapable(String key) {
    final theirs = _heardCaps[key] ?? [if (store.contactByKey(key)?.webrtc ?? false) capWebrtc];
    return mayUse(capWebrtc, caps, theirs);
  }

  /// The tailnet peers the platform shows online, shared with a contact
  /// that asks (protocol section 4.10): Linux's `tailscale status`; none
  /// where the platform has no peer list (Android).
  Future<List<SharedPeer>> Function()? tailnetPeers;

  final List<_Connection> _connections = [];
  final Map<String, DateTime> _typing = {};
  final Map<int, Future<_Connection?>> _dialling = {};
  final Map<String, int> _failures = {};
  final Map<String, DateTime> _nextTry = {};
  final StreamController<ChatEvent> _events = StreamController.broadcast();
  final StreamController<CallAudio> _audio = StreamController.broadcast(sync: true);
  final StreamController<CallRtc> _rtc = StreamController.broadcast(sync: true);
  ServerSocket? _socket;
  HttpServer? _server;
  Timer? _ticker;
  bool _running = false;

  Stream<ChatEvent> get events => _events.stream;

  /// The other side's audio on connected calls, packet by packet: for a
  /// jitter buffer and a decoder.
  Stream<CallAudio> get callAudio => _audio.stream;

  /// The other side's SDP and candidates on WebRTC calls, in order.
  Stream<CallRtc> get callRtc => _rtc.stream;
  PeerAddress get address => PeerAddress(host, port);

  bool get readReceipts => store.setting('read_receipts') != 'off';
  set readReceipts(bool on) => store.setSetting('read_receipts', on ? 'on' : 'off');

  void _emit(ChatEvent e) {
    log?.call('[$name] $e');
    if (!_events.isClosed) _events.add(e);
  }

  // -- lifecycle ----------------------------------------------------------------

  Future<void> start() async {
    final socket = await ServerSocket.bind(host, port);
    port = socket.port;
    _socket = socket;
    _server = HttpServer.listenOn(
      _Admitting(socket, (s) {
        final remote = s.remoteAddress.address;
        if (admitted(remote, loopback: loopback)) return true;
        _emit(ChatEvent('refused', detail: '$remote: not on the tailnet'));
        return false;
      }),
    );
    _server!.listen(_onRequest, onError: (Object _) {});
    _running = true;
    _ticker = Timer.periodic(const Duration(seconds: 1), (_) => _tick());
    _emit(ChatEvent('listening', detail: '$address as ${identity.fingerprint}'));
  }

  Future<void> stop() async {
    _running = false;
    _ticker?.cancel();
    for (final c in List.of(_connections)) {
      c.close(closeGoingAway, 'going away');
    }
    await _server?.close(force: true);
    await _socket?.close();
    _connections.clear();
    await _events.close();
    await _audio.close();
    await _rtc.close();
    store.close();
  }

  // -- contacts -----------------------------------------------------------------

  /// A contact the user chose, by address, by key, or both. Added by
  /// address alone, its key is pinned at the first handshake.
  Contact addContact(PeerAddress? address, {String? name, String? key}) {
    for (final c in store.contacts()) {
      if ((key != null && c.key == key) || (address != null && c.address == address)) return c;
    }
    return store.addContact(name: name ?? address?.toString() ?? key!.substring(0, 8), key: key, address: address);
  }

  /// By key, address, contact name or tailnet name.
  Contact? contact(Object who) {
    for (final c in store.contacts()) {
      if (who is PeerAddress ? c.address == who : (who == c.key || who == c.name || who == c.tailnetName)) return c;
    }
    return null;
  }

  List<Contact> contacts() => store.contacts();
  List<ContactRequest> requests() => store.requests();

  void renameContact(Object who, String newName) => store.renameContact(_contact(who).id, newName);

  /// Unpin a contact: its next message is a contact request again.
  void removeContact(Object who) {
    final c = contact(who);
    if (c != null) store.removeContact(c.id);
    _emit(ChatEvent('contact', key: c?.key, detail: 'removed'));
  }

  Contact _contact(Object who) => contact(who) ?? (throw ArgumentError('no contact $who'));

  /// Accept a contact request: pin its key, show what it held, tell it.
  Contact accept(String key) {
    if (store.request(key) == null) throw ArgumentError('no request from $key');
    final c = store.acceptRequest(key);
    if (_heardCaps[key] case final heard?) store.setWebrtc(c.id, heard.contains(capWebrtc));
    store.queue(key, {'type': 'contact', 'id': newId(), 'state': 'accepted'});
    _emit(ChatEvent('contact', key: key, detail: 'accepted'));
    _flushSoon(key);
    return c;
  }

  /// Decline a contact request: drop what it held and tell it.
  void decline(String key) {
    store.declineRequest(key);
    store.queue(key, {'type': 'contact', 'id': newId(), 'state': 'declined'});
    _emit(ChatEvent('contact', key: key, detail: 'declined'));
    _flushSoon(key);
  }

  /// Put a flagged contact's new key in place of the old one.
  Contact acceptNewKey(Object who) {
    final c = _contact(who);
    if (c.flaggedKey == null) throw ArgumentError('${c.name} has no new key');
    store.pinKey(c.id, c.flaggedKey!);
    _emit(ChatEvent('contact', key: c.flaggedKey, detail: 'new key accepted for ${c.name}'));
    return store.contactById(c.id)!;
  }

  // -- sending ------------------------------------------------------------------

  /// The contact to send to; an address not known yet becomes a contact.
  Contact _resolve(Object who) {
    final c = contact(who) ?? (who is PeerAddress ? addContact(who) : throw ArgumentError('no contact $who'));
    if (c.declinedByThem) throw Refused('${c.name} declined our contact request');
    return c;
  }

  String sendText(Object to, String text, {String? replyTo}) {
    final c = _resolve(to);
    final frame = {'type': 'msg', 'id': newId(), 'ts': nowMs(), 'text': text, if (replyTo != null) 'reply_to': replyTo};
    store.storeMineAndQueue(
      c.box,
      Message(
        id: frame['id'] as String,
        mine: true,
        ts: frame['ts'] as int,
        kind: 'text',
        state: 'waiting',
        text: text,
        replyTo: replyTo,
      ),
      frame,
    );
    _flushSoon(c.box);
    return frame['id'] as String;
  }

  /// React to a message, or remove the reaction with an empty emoji.
  String react(Object to, String messageId, String emoji) {
    final c = _resolve(to);
    final frame = {'type': 'react', 'id': newId(), 'ts': nowMs(), 'to': messageId, 'emoji': emoji};
    store.transaction(() {
      store.applyReaction(c.box, messageId, 'me', emoji, frame['ts'] as int);
      store.queue(c.box, frame);
    });
    _flushSoon(c.box);
    return frame['id'] as String;
  }

  /// Send an Ogg/Opus voice message of `ms` milliseconds.
  String sendVoice(Object to, List<int> data, int ms, {String? replyTo}) {
    if (data.isEmpty || data.length > maxVoiceBytes || ms < 1 || ms > maxVoiceMs) {
      throw ArgumentError('a voice message is 1 to $maxVoiceBytes bytes and 1 to $maxVoiceMs ms');
    }
    final c = _resolve(to);
    final bytes = Uint8List.fromList(data);
    final frame = {
      'type': 'voice',
      'id': newId(),
      'ts': nowMs(),
      'ms': ms,
      'bytes': bytes.length,
      'codec': 'ogg-opus',
      'stream': (randomBytes(4).buffer.asByteData().getUint32(0) | 1),
      if (replyTo != null) 'reply_to': replyTo,
    };
    store.storeMineAndQueue(
      c.box,
      Message(
        id: frame['id'] as String,
        mine: true,
        ts: frame['ts'] as int,
        kind: 'voice',
        state: 'waiting',
        voice: bytes,
        ms: ms,
        replyTo: replyTo,
      ),
      frame,
      data: bytes,
    );
    _flushSoon(c.box);
    return frame['id'] as String;
  }

  /// Read what `who` sent (all of it, or `ids`); a `read` goes only if read
  /// receipts are on, and only on an open connection.
  void markRead(Object who, {List<String>? ids}) {
    final c = _resolve(who);
    final read = store.markRead(c.box, ids: ids);
    if (read.isNotEmpty && readReceipts && c.key != null) {
      for (var i = 0; i < read.length; i += 100) {
        _sendNow(c.key!, {'type': 'read', 'ids': read.sublist(i, (i + 100).clamp(0, read.length))});
      }
    }
  }

  /// Typing on or off: sent on an open connection, never queued.
  void setTyping(Object who, bool on) {
    final c = _resolve(who);
    if (c.key != null) _sendNow(c.key!, {'type': 'typing', 'on': on});
  }

  bool _sendNow(String key, Map<String, Object?> frame) {
    final conn = _connected(key);
    if (conn == null) return false;
    final cap = const {'read': 'receipts', 'typing': 'typing'}[frame['type']];
    if (cap != null && !mayUse(cap, caps, conn.peerCaps)) return false;
    conn.send(frame);
    return true;
  }

  // -- calls: signalling and frames; the codec and devices are CallMedia's ------

  /// Offer a call; its outcome is in [calls]. Unreachable fails at once.
  /// `video`: the caller's camera is on as it rings, said only with webrtc.
  Future<String> call(Object who, {bool video = false}) async {
    final c = _resolve(who);
    final id = newId();
    final stream = randomBytes(4).buffer.asByteData().getUint32(0) | 1;
    calls[id] = Call(peer: c.key ?? '', stream: stream, mine: true, state: 'offered');
    final conn = await _connectionFor(c);
    final transports = conn == null ? const <String>[] : offerTransports(caps, conn.peerCaps);
    if (conn == null) {
      calls[id]!.state = 'ended:unreachable';
    } else if (transports.isEmpty) {
      calls[id] = Call(peer: conn.key, stream: stream, mine: true, state: 'ended:unsupported');
    } else {
      final webrtc = transports.contains(transportWebrtc);
      calls[id] = Call(
        peer: conn.key,
        stream: stream,
        mine: true,
        state: 'offered',
        transports: transports,
        video: webrtc && video,
      );
      conn.send({
        'type': 'call_offer',
        'call': id,
        'transports': transports,
        'stream': stream,
        if (webrtc) 'video': video,
      });
    }
    _emit(ChatEvent('call', key: c.key, id: id, detail: calls[id]!.state));
    return id;
  }

  /// Answer a ringing call, or decline it with a reason.
  void answerCall(String id, {String? decline}) {
    final call = calls[id] ?? (throw ArgumentError('no call $id'));
    final conn = _connected(call.peer);
    final pick = pickTransport(call.transports, caps);
    _answer(
      conn,
      id,
      decline != null || pick == null ? CallAnswer.refuse(decline ?? 'unsupported') : CallAnswer.pick(pick),
    );
  }

  // -- WebRTC calls (protocol 4.11): SDP and candidates are opaque values
  // between the chat connection and the app's engine; no WebRTC here.

  _Connection? _webrtcConnection(Call? call) =>
      call != null && call.state == 'connected' && call.webrtc ? _connected(call.peer) : null;

  /// Send our engine's SDP or one of its candidates; whether it went. A
  /// candidate outside the tailnet is not sent, and an SDP loses those.
  bool sendRtc(String id, RtcPayload payload) {
    final conn = _webrtcConnection(calls[id]);
    if (conn == null) return false;
    final out = switch (payload) {
      RtcCandidate(:final candidate) => _candidateAdmitted(id, candidate, ours: true) ? payload : null,
      RtcSdp(:final type, :final sdp) => RtcSdp(type, tailnetOnlySdp(sdp)),
    };
    if (out == null) return false;
    conn.send({'type': 'call_rtc', 'call': id, ...out.toJson()});
    return true;
  }

  /// Whether a candidate, ours or theirs, is the tailnet's, and so sent or
  /// taken; the log says which, and its kind.
  bool _candidateAdmitted(String id, String line, {required bool ours}) {
    final ok = candidateAdmitted(line);
    final what = ours ? (ok ? 'sent' : 'not sent') : (ok ? 'taken' : 'ignored');
    log?.call('[$name] ${callLabel(id)}: ${ours ? 'our' : 'their'} ${candidateKind(line)} candidate $what');
    return ok;
  }

  /// Tell the other side what our camera is doing.
  bool setCamera(String id, CameraState camera) {
    final conn = _webrtcConnection(calls[id]);
    if (conn == null) return false;
    log?.call('[$name] ${callLabel(id)}: our camera ${camera.wire}');
    conn.send({'type': 'call_camera', 'call': id, 'camera': camera.wire});
    return true;
  }

  /// The engine's media connection changed. It decides the 15 s and 10 s
  /// ends (policy's callEndDue).
  void setMediaState(String id, MediaState state) {
    final call = calls[id];
    if (call == null || !call.webrtc || !call.live || call.media == state) return;
    call.media = state;
    call.mediaSince = DateTime.now();
    if (state == MediaState.connected) call.mediaEverConnected = true;
    log?.call('[$name] ${callLabel(id)}: media ${state.wire}');
  }

  /// One packet of our audio on a connected call.
  void sendCallAudio(String id, int seq, List<int> packet) {
    final call = calls[id];
    if (call == null || call.state != 'connected') return;
    _connected(call.peer)?.sendBinary(kindCallAudio, call.stream, seq, packet);
  }

  /// Hang up (or cancel an offer); a call already over is left as it is.
  void endCall(String id, {String reason = 'hangup'}) {
    final call = calls[id] ?? (throw ArgumentError('no call $id'));
    if (!call.live) return;
    _connected(call.peer)?.send({'type': 'call_end', 'call': id, 'reason': reason});
    call.state = 'ended:$reason';
    _emit(ChatEvent('call', key: call.peer, id: id, detail: call.state));
  }

  // -- reading ------------------------------------------------------------------

  /// A conversation, in the order written. `who` may name a contact or a
  /// requester's key (whose held messages are not shown).
  List<Message> messages(Object who) {
    final c = contact(who);
    return store.conversation(c?.box ?? (who is String ? who : ''));
  }

  /// The ids still waiting in the outbox for `who`.
  List<String> waiting(Object who) {
    final c = contact(who);
    return store.outbox(c?.box ?? (who is String ? who : '')).map((e) => e.id).toList();
  }

  bool isTyping(String key) => policy.isTyping(_typing[key], DateTime.now());

  bool connectedTo(String key) => _connected(key) != null;

  // -- discovery and rejoins ------------------------------------------------------

  /// Whether a chat peer listens at `address`, and who it says it is: a
  /// dial, a `hello`, and a close (protocol section 9). Null if nothing
  /// answers as a chat peer. The key is announced, not proven.
  Future<ProbeResult?> probe(PeerAddress address, {Duration timeout = const Duration(seconds: 3)}) async {
    final h = address.host.contains(':') ? '[${address.host}]' : address.host;
    WebSocket? ws;
    try {
      ws = await WebSocket.connect('ws://$h:${address.port}$wsPath').timeout(timeout);
      ws.add(encodeFrame(helloFrame(identity.key, name, caps, randomBytes(32))));
      await for (final data in ws.timeout(timeout)) {
        final frame = data is String ? checkFrame(data).frame : null;
        if (frame == null || frame['type'] != 'hello') return null;
        final key = frame['key'] as String;
        return ProbeResult(
          address,
          key,
          frame['name'] as String,
          fingerprintOf(fromHex(key)),
          (frame['caps'] as List).cast<String>(),
        );
      }
      return null;
    } on Object {
      return null;
    } finally {
      await ws?.close(closeIdle, 'probe');
    }
  }

  /// Ask contact `who` for its tailnet's peer list (protocol section 4.10),
  /// dialling it if need be. Null if it cannot be reached or either side
  /// does not announce `peers`; a stranger to it gets an empty list. Only
  /// the addresses are to be taken from it, to probe
  /// (`addressesToProbe`).
  Future<List<SharedPeer>?> askPeers(Object who, {Duration timeout = const Duration(seconds: 5)}) async {
    final c = contact(who);
    if (c == null) return null;
    final conn = await _connectionFor(c);
    if (conn == null || !mayUse('peers', caps, conn.peerCaps)) return null;
    final asked = conn.peersAsked ??= Completer<List<SharedPeer>>();
    conn.send(peersAskFrame());
    try {
      return await asked.future.timeout(timeout);
    } on TimeoutException {
      return null;
    } finally {
      if (identical(conn.peersAsked, asked)) conn.peersAsked = null;
    }
  }

  Future<void> _answerPeers(_Connection conn) async {
    final isContact = store.contactByKey(conn.key) != null;
    var online = const <SharedPeer>[];
    if (isContact && tailnetPeers != null) {
      try {
        online = await tailnetPeers!();
      } on Object catch (e) {
        _emit(ChatEvent('peers', key: conn.key, detail: 'no peer list: $e'));
      }
    }
    final shared = sharedPeers(online, asker: conn.remoteHost, contact: isContact);
    _emit(ChatEvent('peers', key: conn.key, detail: 'told ${conn.peerName} ${shared.length} tailnet peers'));
    conn.send(peersFrame(shared));
  }

  /// The tailnet's peer list says these hostnames are online: dial each
  /// contact among them, and each contact with something waiting, so
  /// their outboxes flush. Also what to call after a wake or a network
  /// change.
  Future<void> seenOnline(Set<String> tailnetNames) async {
    final contacts = store.contacts();
    final waiting = store.waitingBoxes().toSet();
    final ids = toDial(
      [
        for (final c in contacts)
          ContactView(
            id: c.id,
            key: c.key,
            address: c.address,
            tailnetName: c.tailnetName,
            declinedByThem: c.declinedByThem,
          ),
      ],
      waiting: {
        for (final c in contacts)
          if (waiting.contains(c.box)) c.id,
      },
      online: tailnetNames,
    );
    Future<void> dial(Contact c) async {
      await _dialling[c.id]; // a dial under way may have started before the peer came back
      final conn = await _connectionFor(c);
      if (conn == null || !_running) return;
      _failures.remove(c.box);
      _nextTry.remove(c.box);
      _flushConnection(conn);
    }

    await Future.wait([
      for (final c in contacts)
        if (ids.contains(c.id)) dial(c),
    ]);
  }

  // -- connections --------------------------------------------------------------

  _Connection? _connected(String key) {
    for (final c in _connections) {
      if (c.authenticated && !c.closed && c.key == key) return c;
    }
    return null;
  }

  /// Dial `address` and shake hands; Refused if refused.
  Future<_Connection> _connect(PeerAddress address) async {
    final h = address.host.contains(':') ? '[${address.host}]' : address.host;
    final ws = await WebSocket.connect('ws://$h:${address.port}$wsPath').timeout(const Duration(seconds: 5));
    final conn = _Connection(this, ws, Role.dialler, address.host, dialled: address);
    _run(conn);
    await conn.ready.future;
    return conn;
  }

  /// An open connection to `c`, dialling its address if need be; null if
  /// it cannot be reached.
  Future<_Connection?> _connectionFor(Contact c) {
    if (c.key != null) {
      final open = _connected(c.key!);
      if (open != null) return Future.value(open);
    }
    final address = c.address;
    if (address == null || !_running) return Future.value(null);
    return _dialling[c.id] ??= () async {
      try {
        return await _connect(address);
      } on Object catch (e) {
        _emit(ChatEvent('unreachable', key: c.key, detail: '${c.name}: $e'));
        return null;
      } finally {
        _dialling.remove(c.id);
      }
    }();
  }

  /// Dial and send the outbox now: for `who`, or for every box waiting.
  Future<void> flush([Object? who]) async {
    final boxes = who == null ? store.waitingBoxes() : [contact(who)?.box ?? who.toString()];
    await Future.wait(boxes.map(_flushBox));
  }

  void _flushSoon(String box) => unawaited(_flushBox(box));

  Future<void> _flushBox(String box) async {
    final c = box.startsWith('contact:') ? store.contactById(int.parse(box.substring(8))) : store.contactByKey(box);
    _Connection? conn = c == null ? _connected(box) : await _connectionFor(c);
    if (!_running) return;
    if (conn == null) {
      final failures = (_failures[box] ?? 0);
      _failures[box] = failures + 1;
      _nextTry[box] = DateTime.now().add(retryDelay(failures));
      return;
    }
    _failures.remove(box);
    _nextTry.remove(box);
    _flushConnection(conn);
  }

  void _flushConnection(_Connection conn) {
    if (conn.closed || !conn.authenticated) return;
    for (final e in store.outbox(conn.key)) {
      if (conn.sentIds.contains(e.id)) continue;
      final frame = Map<String, Object?>.of(e.frame);
      final type = frame['type'];
      if (type == 'react' && !mayUse('react', caps, conn.peerCaps)) continue;
      if (type == 'voice' && !mayUse('voice-message', caps, conn.peerCaps)) continue;
      if (frame.containsKey('reply_to') && !mayUse('reply', caps, conn.peerCaps)) frame.remove('reply_to');
      conn.sentIds.add(e.id);
      conn.send(frame);
      final data = e.data;
      if (data != null) {
        for (var seq = 0, at = 0; at < data.length; seq++, at += maxChunk) {
          conn.sendBinary(
            kindVoice,
            frame['stream'] as int,
            seq,
            data.sublist(at, (at + maxChunk).clamp(0, data.length)),
          );
        }
      }
    }
  }

  void _onRequest(HttpRequest request) async {
    if (request.uri.path != wsPath || !WebSocketTransformer.isUpgradeRequest(request)) {
      request.response.statusCode = HttpStatus.notFound;
      await request.response.close();
      return;
    }
    final remote = request.connectionInfo?.remoteAddress.address ?? '';
    try {
      final ws = await WebSocketTransformer.upgrade(request);
      final conn = _Connection(this, ws, Role.listener, remote);
      _run(conn);
      await conn.ready.future;
    } on Object catch (e) {
      _emit(ChatEvent('refused', detail: '$remote: $e'));
    }
  }

  // -- one connection -----------------------------------------------------------

  void _run(_Connection conn) {
    _connections.add(conn);
    conn.ws.pingInterval = pingInterval;
    conn.handshakeTimer = Timer(handshakeTimeout, () {
      if (!conn.authenticated) conn.close(closeHandshake, 'handshake timeout');
    });
    conn.send(helloFrame(identity.key, name, caps, conn.nonce));
    conn.ws.listen(
      (data) {
        // A connection we closed (a changed key, a duplicate) may still have
        // frames in flight: none of them is acted on.
        if (!_running || conn.closed) return;
        conn.lastActivity = DateTime.now();
        try {
          if (conn.stage != _Stage.open) {
            _onHandshake(conn, data);
          } else if (data is String) {
            _onText(conn, data);
          } else {
            _onBinary(conn, data as List<int>);
          }
        } on Refused catch (e) {
          if (!conn.ready.isCompleted) conn.ready.completeError(e);
        }
      },
      onDone: () => _onClosed(conn),
      onError: (Object _) => _onClosed(conn),
      cancelOnError: true,
    );
  }

  void _onClosed(_Connection conn) {
    final wasOpen = conn.authenticated;
    conn.closed = true;
    conn.handshakeTimer?.cancel();
    conn.transfers.clear(); // a broken voice transfer starts again next time
    _connections.remove(conn);
    if (!conn.ready.isCompleted) {
      conn.ready.completeError(Refused('closed during the handshake (${conn.ws.closeCode})', code: conn.ws.closeCode));
    }
    if (wasOpen) _emit(ChatEvent('disconnected', key: conn.key, detail: '${conn.ws.closeCode}'));
    // A call lives on its connection: with it gone, the call is over.
    if (wasOpen && _connected(conn.key) == null) {
      for (final MapEntry(key: id, value: call) in calls.entries) {
        if (call.live && call.peer == conn.key) {
          call.state = 'ended:error';
          _emit(ChatEvent('call', key: call.peer, id: id, detail: call.state));
        }
      }
    }
  }

  Never _refuse(_Connection conn, String code, String why) {
    conn.send(errorFrame(code, detail: why));
    conn.close(closeHandshake, code);
    throw Refused(why, code: closeHandshake, error: code);
  }

  void _onHandshake(_Connection conn, Object data) {
    final stage = conn.stage == _Stage.hello ? 'hello' : 'auth';
    if (data is! String) _refuse(conn, 'bad_request', 'binary before $stage');
    final checked = checkFrame(data);
    final frame = checked.frame;
    if (frame != null && frame['type'] == 'error') {
      conn.close(closeHandshake, frame['code'] as String);
      throw Refused(
        'refused by the other side: ${frame['code']}',
        code: closeHandshake,
        error: frame['code'] as String,
      );
    }
    if (frame == null || frame['type'] != stage) _refuse(conn, 'bad_request', 'expected $stage');
    if (conn.stage == _Stage.hello) {
      if (frame['version'] != protocolVersion) _refuse(conn, 'version', 'version ${frame['version']}');
      final key = fromHex(frame['key'] as String);
      if (frame['key'] == identity.keyHex) _refuse(conn, 'bad_request', 'my own key');
      conn.peerKey = key;
      conn.peerNonce = fromHex(frame['nonce'] as String);
      conn.peerName = frame['name'] as String;
      conn.peerCaps = (frame['caps'] as List).cast<String>();
      conn.stage = _Stage.auth;
      // The dialler proves itself first; the listener answers only a proven dialler.
      if (conn.role == Role.dialler) conn.send(authFrame(identity.sign(conn.transcriptFor(Role.dialler))));
      return;
    }
    final other = conn.role == Role.dialler ? Role.listener : Role.dialler;
    if (!verifySignature(conn.peerKey!, conn.transcriptFor(other), fromHex(frame['sig'] as String))) {
      _refuse(conn, 'bad_signature', 'bad signature');
    }
    if (conn.role == Role.listener) conn.send(authFrame(identity.sign(conn.transcriptFor(Role.listener))));
    conn.handshakeTimer?.cancel();
    conn.stage = _Stage.open;
    _admit(conn);
  }

  /// Who the proven key is: a contact, a contact's impostor, or a stranger.
  void _admit(_Connection conn) {
    final key = conn.key;
    final contacts = store.contacts();
    final trust = classify(
      key: key,
      peerName: conn.peerName,
      weDialled: conn.role == Role.dialler,
      dialled: conn.dialled,
      remoteHost: conn.remoteHost,
      contacts: [
        for (final c in contacts)
          ContactView(
            id: c.id,
            key: c.key,
            address: c.address,
            tailnetName: c.tailnetName,
            declinedByThem: c.declinedByThem,
          ),
      ],
    );
    String who = 'stranger';
    _heardCaps[key] = conn.peerCaps;
    switch (trust) {
      case KeyMismatch(:final contact):
        final c = store.contactById(contact)!;
        store.flagKey(c.id, key);
        _emit(ChatEvent('key_changed', key: key, detail: '${c.name}: ${fingerprintOf(conn.peerKey!)}'));
        conn.ready.completeError(Refused('key changed for ${c.name}', code: closeKeyChanged, error: 'key_changed'));
        conn.send(errorFrame('key_changed', detail: c.name));
        conn.close(closeKeyChanged, 'key_changed');
        return;
      case PinKey(:final contact):
        store.pinKey(contact, key);
        final c = store.contactById(contact)!;
        _failures.remove('contact:$contact');
        _nextTry.remove('contact:$contact');
        _emit(ChatEvent('contact', key: key, detail: 'pinned ${c.name} as ${fingerprintOf(conn.peerKey!)}'));
        who = 'contact ${c.name}';
        if (c.tailnetName == null) store.setTailnetName(c.id, conn.peerName);
        store.setWebrtc(c.id, conn.peerCaps.contains(capWebrtc));
      case KnownContact(:final contact):
        final c = store.contactById(contact)!;
        who = 'contact ${c.name}';
        if (c.tailnetName == null) store.setTailnetName(c.id, conn.peerName);
        store.setWebrtc(c.id, conn.peerCaps.contains(capWebrtc));
      case Stranger():
        break;
    }
    // One connection per pair: keep the one dialled by the lower key.
    for (final other in List.of(_connections)) {
      if (other == conn || !other.authenticated || other.closed || other.key != key) continue;
      if (keepNewConnection(existingDialledBy: other.dialledBy, newDialledBy: conn.dialledBy)) {
        other.close(closeDuplicate, 'duplicate');
      } else {
        conn.close(closeDuplicate, 'duplicate');
        return; // ready completes with an error; the other stays
      }
    }
    _emit(
      ChatEvent(
        'connected',
        key: key,
        detail: '${conn.role.name} ${conn.peerName} ${fingerprintOf(conn.peerKey!)} ($who)',
      ),
    );
    conn.ready.complete();
    _failures.remove(key);
    _nextTry.remove(key);
    _flushConnection(conn);
  }

  // -- receiving ----------------------------------------------------------------

  void _onText(_Connection conn, String text) {
    final checked = checkFrame(text);
    final frame = checked.frame;
    if (frame == null) {
      _emit(
        ChatEvent('answered', key: conn.key, detail: 'error ${checked.error!['code']} ${checked.error!['what'] ?? ''}'),
      );
      conn.send(checked.error!);
      return;
    }
    if ((frame['type'] == 'peers_ask' || frame['type'] == 'peers') && !mayUse('peers', caps, conn.peerCaps)) {
      _emit(ChatEvent('answered', key: conn.key, detail: 'error unsupported ${frame['type']}'));
      conn.send(errorFrame('unsupported', what: frame['type'] as String));
      return;
    }
    switch (frame['type']) {
      case 'peers_ask':
        unawaited(_answerPeers(conn));
      case 'peers':
        // An unasked list is ignored.
        final asked = conn.peersAsked;
        if (asked != null && !asked.isCompleted) asked.complete(peersOf(frame));
      case 'msg':
        _store(
          conn,
          frame,
          Message(
            id: frame['id'],
            mine: false,
            ts: frame['ts'],
            kind: 'text',
            state: 'received',
            text: frame['text'],
            replyTo: frame['reply_to'],
          ),
        );
      case 'react':
        _onReact(conn, frame);
      case 'voice':
        conn.transfers[frame['stream'] as int] = _Transfer(frame);
      case 'ack':
        _onAck(conn, frame['id'] as String);
      case 'read':
        _onRead(conn, (frame['ids'] as List).cast<String>());
      case 'typing':
        if (store.contactByKey(conn.key) != null) {
          frame['on'] == true ? _typing[conn.key] = DateTime.now() : _typing.remove(conn.key);
          _emit(ChatEvent('typing', key: conn.key, detail: '${frame['on']}'));
        }
      case 'contact':
        _onContact(conn, frame);
      case 'call_offer':
        _onCallOffer(conn, frame);
      case 'call_answer':
        final id = frame['call'] as String;
        final call = calls[id];
        if (call != null && call.peer == conn.key && call.state == 'offered') {
          call.transport = frame['transport'] as String?;
          call.state = frame.containsKey('transport') ? 'connected' : 'declined:${frame['decline']}';
          call.lastFrame = DateTime.now();
          if (call.transport != null) log?.call('[$name] ${callLabel(id)} over ${call.transport}');
          _emit(ChatEvent('call', key: conn.key, id: id, detail: call.state));
        }
      case 'call_rtc':
        _onCallRtc(conn, frame);
      case 'call_camera':
        // One for a call that is not a WebRTC call of this connection is ignored.
        final id = frame['call'] as String;
        final call = calls[id];
        if (_webrtcConnection(call) == conn) {
          final camera = call!.remoteCamera = CameraState.fromWire(frame['camera'])!;
          log?.call('[$name] ${callLabel(id)}: their camera ${camera.wire}');
          _emit(ChatEvent('call', key: conn.key, id: id, detail: 'camera ${camera.wire}'));
        }
      case 'call_end':
        final call = calls[frame['call']];
        if (call != null && call.peer == conn.key && call.live) {
          call.state = 'ended:${frame['reason']}';
          _emit(ChatEvent('call', key: conn.key, id: frame['call'] as String, detail: call.state));
        }
      case 'error':
        _emit(ChatEvent('error', key: conn.key, detail: '${frame['code']} ${frame['what'] ?? ''}'));
      default: // hello, auth
        conn.send(errorFrame('bad_request', what: frame['type'] as String, detail: 'after the handshake'));
    }
  }

  /// A message, reaction or voice message: stored once, acked every time.
  /// A stranger's is held as a contact request.
  void _store(_Connection conn, Map<String, dynamic> frame, Message m) {
    final key = conn.key;
    final isContact = store.contactByKey(key) != null;
    if (!isContact) {
      store.addRequest(
        key: key,
        name: conn.peerName ?? '',
        host: conn.remoteHost,
        fingerprint: fingerprintOf(conn.peerKey!),
        now: nowMs(),
      );
    }
    final fresh = store.storeReceived(key, m, held: !isContact);
    if (fresh && isContact && m.kind == 'react') store.applyReaction(key, m.reactTo!, 'them', m.emoji!, m.ts);
    conn.send({'type': 'ack', 'id': frame['id']});
    if (fresh)
      _emit(ChatEvent(isContact ? (m.kind == 'react' ? 'reaction' : 'message') : 'request', key: key, id: m.id));
  }

  void _onReact(_Connection conn, Map<String, dynamic> frame) => _store(
    conn,
    frame,
    Message(
      id: frame['id'],
      mine: false,
      ts: frame['ts'],
      kind: 'react',
      state: 'received',
      reactTo: frame['to'],
      emoji: frame['emoji'],
    ),
  );

  void _onAck(_Connection conn, String id) {
    final key = conn.key;
    store.acked(key, id);
    store.setMineState(key, id, 'delivered');
    _emit(ChatEvent('delivered', key: key, id: id));
  }

  void _onRead(_Connection conn, List<String> ids) {
    if (store.contactByKey(conn.key) == null) return;
    for (final id in ids) {
      store.setMineState(conn.key, id, 'read', from: const ['waiting', 'delivered']);
    }
    _emit(ChatEvent('read', key: conn.key, detail: ids.join(',')));
  }

  void _onContact(_Connection conn, Map<String, dynamic> frame) {
    final c = store.contactByKey(conn.key);
    if (c != null) {
      final accepted = frame['state'] == 'accepted';
      store.setAnswer(c.id, accepted: accepted);
      if (!accepted) store.dropOutbox(conn.key);
    }
    conn.send({'type': 'ack', 'id': frame['id']});
    _emit(ChatEvent('contact', key: conn.key, detail: '${frame['state']} by them'));
  }

  void _onBinary(_Connection conn, List<int> data) {
    final BinaryFrame b;
    try {
      b = unpackBinary(data);
    } on FormatException catch (e) {
      conn.send(errorFrame('bad_request', what: 'binary', detail: e.message));
      return;
    }
    if (b.kind == kindVoice) {
      final t = conn.transfers[b.stream];
      if (t == null || b.seq != t.next) {
        conn.transfers.remove(b.stream);
        conn.send(
          errorFrame('bad_request', what: 'binary', detail: 'no voice transfer at stream ${b.stream} seq ${b.seq}'),
        );
        return;
      }
      t.data.add(b.payload);
      t.next++;
      final want = t.frame['bytes'] as int;
      if (t.data.length > want) {
        conn.transfers.remove(b.stream);
        conn.send(
          errorFrame('bad_request', what: 'voice', id: t.frame['id'] as String, detail: 'more bytes than announced'),
        );
      } else if (t.data.length == want) {
        conn.transfers.remove(b.stream);
        final f = t.frame;
        _store(
          conn,
          f,
          Message(
            id: f['id'],
            mine: false,
            ts: f['ts'],
            kind: 'voice',
            state: 'received',
            voice: t.data.takeBytes(),
            ms: f['ms'],
            replyTo: f['reply_to'],
          ),
        );
      }
    } else if (b.kind == kindCallAudio) {
      for (final MapEntry(key: id, value: call) in calls.entries) {
        if (call.stream == b.stream && call.peer == conn.key && call.state == 'connected') {
          call.frames++;
          call.lastFrame = DateTime.now();
          if (!_audio.isClosed) _audio.add(CallAudio(id, b.seq, b.payload));
        }
      }
    } else {
      conn.send(errorFrame('unsupported', what: 'binary kind ${b.kind}'));
    }
  }

  /// The other side's SDP or candidate, for the engine. One for a call that
  /// is not a WebRTC call of this connection, or a candidate outside the
  /// tailnet, is ignored; an SDP loses its candidates outside it.
  void _onCallRtc(_Connection conn, Map<String, dynamic> frame) {
    final id = frame['call'] as String;
    if (_webrtcConnection(calls[id]) != conn) return;
    final payload = switch (RtcPayload.fromFrame(frame)) {
      final RtcCandidate c => _candidateAdmitted(id, c.candidate, ours: false) ? c : null,
      RtcSdp(:final type, :final sdp) => RtcSdp(type, tailnetOnlySdp(sdp)),
    };
    if (payload == null || _rtc.isClosed) return;
    _rtc.add(CallRtc(id, payload));
  }

  void _onCallOffer(_Connection conn, Map<String, dynamic> frame) {
    final id = frame['call'] as String;
    final busy = calls.values.any((c) => c.live);
    final transports = (frame['transports'] as List).cast<String>();
    calls[id] = Call(
      peer: conn.key,
      stream: frame['stream'] as int,
      mine: false,
      state: 'ringing',
      transports: transports,
      video: frame['video'] == true,
    );
    final answer = answerOffer(
      isContact: store.contactByKey(conn.key) != null,
      busy: busy,
      transports: transports,
      caps: caps,
      policy: callPolicy,
    );
    _emit(ChatEvent('call', key: conn.key, id: id, detail: 'ringing'));
    if (!answer.ringing) _answer(conn, id, answer);
  }

  void _answer(_Connection? conn, String id, CallAnswer answer) {
    final call = calls[id]!;
    call.transport = answer.transport;
    call.state = answer.transport != null ? 'connected' : 'declined:${answer.decline}';
    if (answer.transport != null) log?.call('[$name] ${callLabel(id)} over ${answer.transport}');
    call.lastFrame = DateTime.now();
    conn?.send({
      'type': 'call_answer',
      'call': id,
      if (answer.transport != null) 'transport': answer.transport else 'decline': answer.decline,
    });
    _emit(ChatEvent('call', key: call.peer, id: id, detail: call.state));
  }

  // -- the ticker: retries, idle connections, call timeouts ---------------------

  void _tick() {
    if (!_running) return;
    final now = DateTime.now();
    for (final box in store.waitingBoxes()) {
      final due = _nextTry[box];
      final key = box.startsWith('contact:') ? null : box;
      if (key != null && _connected(key) != null) continue;
      if (due == null || !now.isBefore(due)) _flushSoon(box);
    }
    for (final MapEntry(key: id, value: call) in List.of(calls.entries)) {
      if (callEndDue(
            state: call.state,
            since: call.since,
            lastFrame: call.lastFrame,
            now: now,
            transport: call.transport,
            media: call.media,
            mediaSince: call.mediaSince,
            mediaEverConnected: call.mediaEverConnected,
          )
          case final why?) {
        endCall(id, reason: why);
      }
    }
    final inCall = {
      for (final c in calls.values)
        if (c.live) c.peer,
    };
    for (final conn in List.of(_connections)) {
      if (!conn.authenticated || conn.closed) continue;
      final busy = store.outbox(conn.key).isNotEmpty || inCall.contains(conn.key) || conn.transfers.isNotEmpty;
      if (!busy && now.difference(conn.lastActivity) > idleTimeout) conn.close(closeIdle, 'idle');
    }
  }
}

/// The listening socket, closing anything not admitted before HTTP.
class _Admitting extends StreamView<Socket> implements ServerSocket {
  _Admitting(this._inner, bool Function(Socket) admit)
    : super(
        _inner.where((s) {
          bool ok;
          try {
            ok = admit(s);
          } on Object {
            ok = false;
          }
          if (!ok) s.destroy();
          return ok;
        }),
      );

  final ServerSocket _inner;
  @override
  InternetAddress get address => _inner.address;
  @override
  int get port => _inner.port;
  @override
  Future<ServerSocket> close() => _inner.close();
}

/// The JSON a UI or the control protocol shows for a message.
Map<String, Object?> messageJson(Message m) => {
  'id': m.id,
  'mine': m.mine,
  'ts': m.ts,
  'state': m.state,
  if (m.text != null) 'text': m.text,
  if (m.replyTo != null) 'reply_to': m.replyTo,
  if (m.voice != null) 'voice_bytes': m.voice!.length,
  if (m.ms != null) 'ms': m.ms,
  if (m.reactions.isNotEmpty) 'reactions': m.reactions,
};
