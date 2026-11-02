/// The tailnet as `tailscale status --json` tells it: whether Tailscale
/// runs, our tailnet address, and the peers online. And discovery: which of
/// those peers answer as chat peers (spec, The companion app: Discovery).
///
/// On Android there is no `tailscale` command and no peer list: the
/// Tailscale app is a VPN, and its interface's address is all there is
/// to read ([statusFromInterfaces]). Peers are added there by name or
/// address, dialled from the contacts already known, and looked for among
/// the peer lists contacts share ([discoverThroughContacts]).
library;

import 'dart:async';
import 'dart:convert';
import 'dart:io';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';

enum TailnetState {
  running,
  starting,
  stopped,
  needsLogin,
  notRunning,
  notInstalled,

  /// Android: no VPN interface with a tailnet address.
  appNotConnected,
}

class TailnetPeer {
  const TailnetPeer({required this.hostName, required this.dnsName, required this.ips});

  /// The machine's own hostname, as a chat peer on it may announce.
  final String hostName;

  /// The first label of its MagicDNS name, `pixel-8`; '' if none.
  final String dnsName;
  final List<String> ips;

  /// Where to dial it: its IPv4 address, else its first.
  String get address => ips.firstWhere((ip) => !ip.contains(':'), orElse: () => ips.first);
}

class TailscaleStatus {
  const TailscaleStatus(
    this.state, {
    this.selfName,
    this.selfIps = const [],
    this.peers = const [],
    this.authUrl,
    this.hasPeerList = true,
  });

  final TailnetState state;
  final String? selfName;
  final List<String> selfIps;

  /// Online peers only, with at least one address.
  final List<TailnetPeer> peers;
  final String? authUrl;

  /// Whether [peers] is the tailnet's peer list (Linux), so discovery can
  /// probe it; false on Android, where nothing lists the peers.
  final bool hasPeerList;

  /// The address to listen on: our tailnet IPv4 address, while running.
  String? get address {
    if (state != TailnetState.running || selfIps.isEmpty) return null;
    return selfIps.firstWhere((ip) => !ip.contains(':'), orElse: () => selfIps.first);
  }

  /// What a contact that asks is told of the tailnet (protocol section
  /// 4.10): the online peers, by MagicDNS name; none without a peer list.
  List<SharedPeer> get toShare => [
    if (hasPeerList)
      for (final p in peers) SharedPeer(p.dnsName.isNotEmpty ? p.dnsName : p.hostName, p.ips),
  ];

  /// Every name an online peer may be known by, for `ChatPeer.seenOnline`.
  Set<String> get onlineNames => {
    for (final p in peers) ...[p.hostName, if (p.dnsName.isNotEmpty) p.dnsName],
  };

  /// What to tell the user when chat cannot work; null while running.
  String? get problem => switch (state) {
    TailnetState.running => null,
    TailnetState.starting => 'Tailscale is starting.',
    TailnetState.stopped => 'Tailscale is stopped. Start it with `tailscale up`.',
    TailnetState.needsLogin =>
      'Tailscale needs you to sign in${authUrl == null || authUrl!.isEmpty ? '' : ' at $authUrl'}.',
    TailnetState.notRunning => 'Tailscale is not running, so nothing can connect. Start tailscaled.',
    TailnetState.notInstalled => 'Tailscale is not installed: the `tailscale` command was not found.',
    TailnetState.appNotConnected =>
      'The Tailscale app is not connected, so nothing can reach this phone. Open Tailscale and connect.',
  };
}

/// Parse `tailscale status --json`. Anything that is not its JSON means
/// the daemon did not answer.
TailscaleStatus parseTailscaleStatus(String text) {
  final Object? json;
  try {
    json = jsonDecode(text);
  } on FormatException {
    return const TailscaleStatus(TailnetState.notRunning);
  }
  if (json is! Map<String, dynamic>) return const TailscaleStatus(TailnetState.notRunning);
  final state = switch (json['BackendState']) {
    'Running' => TailnetState.running,
    'Starting' => TailnetState.starting,
    'Stopped' => TailnetState.stopped,
    'NeedsLogin' || 'NeedsMachineAuth' => TailnetState.needsLogin,
    _ => TailnetState.notRunning,
  };
  final self = json['Self'] is Map<String, dynamic> ? json['Self'] as Map<String, dynamic> : const {};
  final peers = <TailnetPeer>[];
  if (state == TailnetState.running && json['Peer'] is Map<String, dynamic>) {
    for (final p in (json['Peer'] as Map<String, dynamic>).values) {
      if (p is! Map<String, dynamic> || p['Online'] != true) continue;
      final ips = _strings(p['TailscaleIPs']);
      if (ips.isEmpty) continue;
      peers.add(TailnetPeer(hostName: '${p['HostName'] ?? ''}', dnsName: _firstLabel(p['DNSName']), ips: ips));
    }
  }
  return TailscaleStatus(
    state,
    selfName: self['HostName'] as String?,
    selfIps: _strings(json['TailscaleIPs'] ?? self['TailscaleIPs']),
    peers: peers,
    authUrl: json['AuthURL'] as String?,
  );
}

List<String> _strings(Object? v) => v is List ? [for (final s in v) '$s'] : const [];

String _firstLabel(Object? dns) => dns is String && dns.isNotEmpty ? dns.split('.').first : '';

typedef RunProcess = Future<ProcessResult> Function(String executable, List<String> arguments);

/// Run `tailscale status --json` and parse it.
Future<TailscaleStatus> readTailscaleStatus({RunProcess run = Process.run}) async {
  try {
    final r = await run('tailscale', ['status', '--json']);
    // `tailscale status` exits non-zero when stopped but still prints the JSON.
    return parseTailscaleStatus('${r.stdout}');
  } on ProcessException {
    return const TailscaleStatus(TailnetState.notInstalled);
  }
}

// -- Android ----------------------------------------------------------------------

/// Whether `ip` is in 100.64.0.0/10, where Tailscale gives its addresses.
bool isTailnetAddress(String ip) {
  final a = InternetAddress.tryParse(ip);
  if (a == null || a.type != InternetAddressType.IPv4) return false;
  final b = a.rawAddress;
  return b[0] == 100 && b[1] >= 64 && b[1] < 128;
}

/// The tailnet as Android shows it: the Tailscale app connected is a VPN
/// interface (`tun0`) holding a 100.64.0.0/10 address. That range is also
/// carrier-grade NAT, so such an address on the mobile interface does not
/// count. `addresses` is each interface's addresses, by name.
TailscaleStatus statusFromInterfaces(Map<String, List<String>> addresses) {
  for (final MapEntry(key: name, value: ips) in addresses.entries) {
    if (!name.startsWith('tun')) continue;
    final ip = ips.where(isTailnetAddress).firstOrNull;
    if (ip != null) {
      return TailscaleStatus(TailnetState.running, selfIps: [ip, ...ips.where((a) => a != ip)], hasPeerList: false);
    }
  }
  return const TailscaleStatus(TailnetState.appNotConnected, hasPeerList: false);
}

Future<Map<String, List<String>>> _listInterfaces() async => {
  for (final i in await NetworkInterface.list(includeLinkLocal: false))
    i.name: [for (final a in i.addresses) a.address],
};

/// Read the tailnet on Android from the network interfaces.
Future<TailscaleStatus> readAndroidTailnet({
  Future<Map<String, List<String>>> Function() list = _listInterfaces,
}) async {
  try {
    return statusFromInterfaces(await list());
  } on SocketException {
    return const TailscaleStatus(TailnetState.appNotConnected, hasPeerList: false);
  }
}

class DiscoveredPeer {
  const DiscoveredPeer(this.peer, this.probe, {this.via});
  final TailnetPeer peer;
  final ProbeResult probe;

  /// The contact whose peer list it was found in (Android); null when it
  /// came from our own.
  final String? via;
}

/// Probe each online tailnet peer's chat port with a hello, all at once,
/// and keep those that answer. `probe` is `ChatPeer.probe`.
Future<List<DiscoveredPeer>> discoverChatPeers(
  TailscaleStatus status,
  Future<ProbeResult?> Function(PeerAddress address) probe, {
  int port = defaultPort,
}) async {
  final answers = await Future.wait([
    for (final p in status.peers)
      probe(PeerAddress(p.address, port)).then((r) => r == null ? null : DiscoveredPeer(p, r)),
  ]);
  return [for (final a in answers) ?a];
}

/// Android, which has no peer list of its own: ask each contact for its
/// tailnet's peers (`ChatPeer.askPeers`), all at once, and probe each
/// address they share, once, as [discoverChatPeers] does. Only addresses
/// inside the tailnet's ranges are taken from what they share, never a name
/// or a key; ours and the contacts' are not probed. Each chat peer found
/// says which contact shared it.
Future<List<DiscoveredPeer>> discoverThroughContacts(
  List<Contact> contacts,
  Future<List<SharedPeer>?> Function(Contact contact) ask,
  Future<ProbeResult?> Function(PeerAddress address) probe, {
  int port = defaultPort,
  Set<String> ownAddresses = const {},
}) async {
  final lists = await Future.wait([
    for (final c in contacts)
      ask(c).then<List<SharedPeer>?>((l) => l, onError: (Object _) => null).then((l) => (c, l ?? const <SharedPeer>[])),
  ]);
  final known = {...ownAddresses, for (final c in contacts) ?c.address?.host};
  final via = <String, String>{};
  for (final (c, shared) in lists) {
    for (final a in addressesToProbe(shared)) {
      if (!known.contains(a)) via.putIfAbsent(a, () => c.name);
    }
  }
  final answers = await Future.wait([
    for (final MapEntry(key: a, value: name) in via.entries)
      probe(PeerAddress(a, port)).then(
        (r) => r == null
            ? null
            : DiscoveredPeer(
                TailnetPeer(hostName: '', dnsName: '', ips: [a]),
                r,
                via: name,
              ),
      ),
  ]);
  return [for (final a in answers) ?a];
}
