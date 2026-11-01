/// The app's state over the core's [ChatPeer]: whether Tailscale runs and
/// where we listen, discovery, the conversation on screen, typing,
/// notifications, and the settings. The protocol, the policy, the outbox
/// and the storage are the core's; this is platform glue the screens read.
///
/// A conversation is named by its box (the contact's key, or `contact:<id>`
/// before its key is pinned), as the core's store names it.
library;

import 'dart:async';
import 'dart:io';

import 'package:flutter/foundation.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';

import 'background.dart';
import 'notifier.dart';
import 'permissions.dart';
import 'tailnet.dart';

/// `host`, `host:port`, `[v6]:port` or a bare IPv6 address.
(String, int?) parsePeerInput(String input) {
  final s = input.trim();
  if (s.isEmpty) throw ArgumentError('a name or an address');
  final bracketed = RegExp(r'^\[([^\]]+)\](?::(\d+))?$').firstMatch(s);
  if (bracketed != null) return (bracketed[1]!, bracketed[2] == null ? null : int.parse(bracketed[2]!));
  if (':'.allMatches(s).length > 1) return (s, null); // IPv6 without a port
  final colon = s.lastIndexOf(':');
  if (colon < 0) return (s, null);
  final port = int.tryParse(s.substring(colon + 1));
  if (port == null || port < 1 || port > 65535) throw ArgumentError('$s: not a port');
  return (s.substring(0, colon), port);
}

typedef Lookup = Future<List<InternetAddress>> Function(String host);

class ChatModel extends ChangeNotifier {
  ChatModel({
    required this.identity,
    required this.openStore,
    required this.name,
    this.readStatus = readTailscaleStatus,
    Notifier? notifier,
    this.listenHost,
    this.port = defaultPort,
    this.loopback = false,
    this.discoveryPort = defaultPort,
    this.refreshEvery = const Duration(seconds: 30),
    this.lookup = InternetAddress.lookup,
    this.log,
    this.background,
    this.permissions,
  }) : notifier = notifier ?? _Silent();

  final Identity identity;
  final Store Function() openStore;
  final String name;
  final Future<TailscaleStatus> Function() readStatus;
  final Notifier notifier;

  /// Listen here rather than on the tailnet address (tests; a second instance).
  final String? listenHost;
  final int port;
  final bool loopback;

  /// The chat port probed on other machines, and dialled for a name typed in.
  final int discoveryPort;
  final Duration refreshEvery;
  final Lookup lookup;
  final void Function(String line)? log;

  /// Android's foreground service; null on Linux.
  final BackgroundService? background;

  /// Android's runtime permissions; null on Linux, which asks for none.
  final Permissions? permissions;

  TailscaleStatus status = const TailscaleStatus(TailnetState.starting);
  late ChatPeer _peer = _newPeer('127.0.0.1');
  bool _started = false;
  StreamSubscription<ChatEvent>? _events;
  Timer? _refresh;
  Timer? _tick;
  bool _closed = false;

  List<DiscoveredPeer> _discovered = [];
  bool discovering = false;
  String? _open;
  bool _foreground = true;
  final Map<String, DateTime> _typingSent = {};
  Set<String> _typingShown = {};

  /// The last reason each contact could not be reached, until it connects.
  final Map<String, String> unreachable = {};

  ChatPeer get peer => _peer;

  /// Where we listen, once Tailscale runs.
  PeerAddress? get listening => _started ? _peer.address : null;

  /// Why chat cannot work now, for a banner; null when it can.
  String? get problem => status.problem;

  // -- lifecycle ------------------------------------------------------------------

  ChatPeer _newPeer(String host) {
    final p = ChatPeer(
      identity: identity,
      store: openStore(),
      name: name,
      host: host,
      port: port,
      loopback: loopback,
      log: log,
    );
    _events = p.events.listen(_onEvent);
    return p;
  }

  Future<void> start() async {
    await refresh();
    // Not awaited: the notification prompt waits on the user, the screens do not.
    unawaited(_applyBackground());
    if (refreshEvery > Duration.zero) _refresh = Timer.periodic(refreshEvery, (_) => refresh());
    _tick = Timer.periodic(const Duration(seconds: 1), (_) => _typingLapse());
  }

  /// Read Tailscale's status again: listen on its address if it changed,
  /// and dial the contacts it shows online, so their outboxes flush.
  Future<void> refresh() async {
    final was = status.address;
    status = await readStatus();
    if (_closed) return;
    final host = status.state == TailnetState.running ? (listenHost ?? status.address) : null;
    // Without a peer list (Android), the tailnet coming up or moving is
    // when to dial the contacts known.
    final dialKnown = !status.hasPeerList && status.address != null && status.address != was;
    if (host != null && (!_started || host != _peer.host)) {
      if (_started || host != _peer.host) {
        final old = _peer;
        await _events?.cancel();
        _started = false;
        await old.stop();
        _peer = _newPeer(host);
      }
      try {
        await _peer.start();
        _started = true;
      } on SocketException catch (e) {
        log?.call('cannot listen on $host:$port: $e');
      }
    }
    if (_started) unawaited(_peer.seenOnline(dialKnown ? _knownNames : status.onlineNames));
    notifyListeners();
  }

  /// The contacts' tailnet names: who to dial when nothing says who is online.
  Set<String> get _knownNames => {for (final c in contacts) ?c.tailnetName};

  /// Start or stop the background service as the setting says; it shows a
  /// notification, so notifications are asked for first.
  /// One after another, so a switch flipped during the prompt lands last.
  Future<void> _applyBackground() => _background = _background.then((_) async {
    final service = background;
    if (service == null || _closed) return;
    final on = backgroundListening;
    if (on) await permissions?.need(AppPermission.notifications);
    await service.setRunning(on);
  });
  Future<void> _background = Future.value();

  Future<void> close() async {
    _closed = true;
    _refresh?.cancel();
    _tick?.cancel();
    await _events?.cancel();
    await _peer.stop();
  }

  // -- events, notifications, reading -------------------------------------------

  /// Whether the window is in front; set from the app's lifecycle.
  bool get foreground => _foreground;
  set foreground(bool on) {
    final back = on && !_foreground;
    _foreground = on;
    if (on && _open != null) _markRead(_open!);
    // Without a peer list, coming to the front is when to look for the
    // contacts again: a message held for us while we were away comes now.
    if (back && _started && !status.hasPeerList) unawaited(_peer.seenOnline(_knownNames));
  }

  String? get openConversationBox => _open;

  void openConversation(String box) {
    _open = box;
    if (_foreground) _markRead(box);
    notifyListeners();
  }

  void closeConversation() {
    if (_open != null) _stopTyping(_open!);
    _open = null;
  }

  void _markRead(String box) {
    final c = _contactByBox(box);
    if (c != null) _peer.markRead(_who(c));
  }

  void _onEvent(ChatEvent e) {
    switch (e.kind) {
      case 'message':
        final c = e.key == null ? null : _peer.contact(e.key!);
        if (c == null) break;
        if (_foreground && _open == c.box) {
          _markRead(c.box);
        } else {
          final m = _peer.messages(c.box).where((m) => m.id == e.id).firstOrNull;
          unawaited(notifier.show(c.name, _preview(m), conversation: c.box));
        }
      case 'request':
        if (!_foreground) {
          final r = requests.where((r) => r.key == e.key).firstOrNull;
          if (r != null) {
            unawaited(
              notifier.show('Contact request from ${r.name}', _preview(r.held.where((m) => m.id == e.id).firstOrNull)),
            );
          }
        }
      case 'call':
        final call = e.id == null ? null : _peer.calls[e.id!];
        if (call != null && !call.mine && call.state == 'ringing' && !_foreground) {
          unawaited(notifier.show('Call from ${_peer.contact(call.peer)?.name ?? 'a contact'}', 'Ringing'));
        }
      case 'unreachable':
        if (e.key != null) unreachable[e.key!] = e.detail ?? 'unreachable';
      case 'connected':
        if (e.key != null) unreachable.remove(e.key);
    }
    notifyListeners();
  }

  static String _preview(Message? m) => switch (m) {
    null => '',
    Message(kind: 'voice', :final ms) => 'Voice message, ${((ms ?? 0) / 1000).round()} s',
    Message(:final text) => text ?? '',
  };

  void _typingLapse() {
    final now = {
      for (final c in contacts)
        if (c.key != null && _peer.isTyping(c.key!)) c.key!,
    };
    if (!setEquals(now, _typingShown)) {
      _typingShown = now;
      notifyListeners();
    }
  }

  // -- contacts and requests ----------------------------------------------------

  List<Contact> get contacts => _peer.contacts();
  List<ContactRequest> get requests => _peer.requests();

  Contact? _contactByBox(String box) => contacts.where((c) => c.box == box || c.key == box).firstOrNull;

  Contact _contact(String box) => _contactByBox(box) ?? (throw ArgumentError('no contact $box'));

  /// How to name a contact to the core: by key once pinned, else by address.
  Object _who(Contact c) => c.key ?? c.address ?? c.name;

  /// Contacts by their latest message, newest first; those with none last.
  List<(Contact, Message?)> get conversations {
    final list = [for (final c in contacts) (c, _peer.messages(c.box).where((m) => m.kind != 'react').lastOrNull)];
    list.sort((a, b) => (b.$2?.ts ?? 0).compareTo(a.$2?.ts ?? 0));
    return list;
  }

  Contact addContact(PeerAddress address, {String? name}) {
    final c = _peer.addContact(address, name: name);
    _wantNotifications();
    notifyListeners();
    return c;
  }

  /// A contact can now message us: notifications are first needed.
  void _wantNotifications() {
    if (permissions != null) unawaited(permissions!.need(AppPermission.notifications));
  }

  /// A peer typed in by tailnet name, MagicDNS name, DNS name or address,
  /// with a port or not.
  Future<Contact> addByName(String input) async {
    final (host, port) = parsePeerInput(input);
    final lower = host.toLowerCase();
    final known = status.peers
        .where((p) => p.hostName.toLowerCase() == lower || p.dnsName.toLowerCase() == lower)
        .firstOrNull;
    String address;
    if (known != null) {
      address = known.address;
    } else if (InternetAddress.tryParse(host) != null) {
      address = host;
    } else {
      try {
        final found = await lookup(host);
        if (found.isEmpty) throw ArgumentError('$host: no address');
        address = found.first.address;
      } on SocketException {
        throw ArgumentError('$host: not a tailnet peer, and no address found for it');
      }
    }
    return addContact(PeerAddress(address, port ?? discoveryPort), name: known?.hostName ?? host);
  }

  Contact accept(String key) {
    final c = _peer.accept(key);
    _wantNotifications();
    notifyListeners();
    return c;
  }

  void decline(String key) {
    _peer.decline(key);
    notifyListeners();
  }

  void acceptNewKey(String box) {
    _peer.acceptNewKey(_who(_contact(box)));
    notifyListeners();
  }

  void rename(String box, String newName) {
    _peer.renameContact(_who(_contact(box)), newName);
    notifyListeners();
  }

  void remove(String box) {
    _peer.removeContact(_who(_contact(box)));
    if (_open == box) _open = null;
    notifyListeners();
  }

  // -- discovery ------------------------------------------------------------------

  /// Chat peers found among the online tailnet peers, less the contacts.
  List<DiscoveredPeer> get discovered => [
    for (final d in _discovered)
      if (!contacts.any((c) => c.key == d.probe.key || c.address == d.probe.address)) d,
  ];

  Future<void> discover() async {
    discovering = true;
    notifyListeners();
    try {
      status = await readStatus();
      _discovered = await discoverChatPeers(status, (a) => _peer.probe(a), port: discoveryPort);
    } finally {
      discovering = false;
      notifyListeners();
    }
  }

  Contact addDiscovered(DiscoveredPeer d) => addContact(d.probe.address, name: d.probe.name);

  // -- messages -------------------------------------------------------------------

  List<Message> messages(String box) => _peer.messages(box).where((m) => m.kind != 'react').toList();

  /// The message a reply quotes, if this conversation has it.
  Message? quoted(String box, String id) => _peer.messages(box).where((m) => m.id == id).firstOrNull;

  List<String> waiting(String box) => _peer.waiting(box);

  String send(String box, String text, {String? replyTo}) {
    final c = _contact(box);
    _stopTyping(box);
    final id = _peer.sendText(_who(c), text, replyTo: replyTo);
    notifyListeners();
    return id;
  }

  /// React to a message; '' removes our reaction.
  void react(String box, String messageId, String emoji) {
    _peer.react(_who(_contact(box)), messageId, emoji);
    notifyListeners();
  }

  /// The compose field changed: tell them we type, again every 4 s while we
  /// do (it lapses after 6 s), and that we stopped when it is empty.
  void typed(String box, String text) {
    final c = _contactByBox(box);
    if (c?.key == null) return;
    if (text.isEmpty) return _stopTyping(box);
    final last = _typingSent[box];
    final now = DateTime.now();
    if (last == null || now.difference(last) >= const Duration(seconds: 4)) {
      _typingSent[box] = now;
      _peer.setTyping(c!.key!, true);
    }
  }

  void _stopTyping(String box) {
    if (_typingSent.remove(box) == null) return;
    final c = _contactByBox(box);
    if (c?.key != null) _peer.setTyping(c!.key!, false);
  }

  bool isTyping(String box) {
    final key = _contactByBox(box)?.key;
    return key != null && _peer.isTyping(key);
  }

  bool connectedTo(String box) {
    final key = _contactByBox(box)?.key;
    return key != null && _peer.connectedTo(key);
  }

  // -- settings -------------------------------------------------------------------

  bool get readReceipts => _peer.readReceipts;
  set readReceipts(bool on) {
    _peer.readReceipts = on;
    notifyListeners();
  }

  /// Keep listening while the app is in the background: Android's
  /// foreground service (ticket 11). On Linux the app listens while it runs.
  bool get backgroundListening => _peer.store.setting('background_listening') != 'off';
  set backgroundListening(bool on) {
    _peer.store.setSetting('background_listening', on ? 'on' : 'off');
    unawaited(_applyBackground());
    notifyListeners();
  }
}

class _Silent implements Notifier {
  @override
  Future<void> show(String title, String body, {String? conversation}) async {}
}
