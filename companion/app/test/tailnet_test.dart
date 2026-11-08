import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:thistle_chat/src/tailnet.dart';

String fixture(String name) => File('test/fixtures/$name').readAsStringSync();

void main() {
  group('parseTailscaleStatus', () {
    test('a running tailnet: our address, and the online peers only', () {
      final s = parseTailscaleStatus(fixture('tailscale_status_running.json'));
      expect(s.state, TailnetState.running);
      expect(s.selfName, 'laptop');
      expect(s.address, '100.91.10.1');
      expect(s.peers.map((p) => p.hostName), ['thistle-46cf20', 'homelab', 'Pixel 8']);
      final device = s.peers.first;
      expect(device.dnsName, 'thistle-46cf20');
      expect(device.address, '100.110.0.38');
      // A peer with only an IPv6 address is dialled on it.
      expect(s.peers.last.address, 'fd7a:115c:a1e0::ee05');
    });

    test('the names a peer may announce: its hostname and its MagicDNS name', () {
      final s = parseTailscaleStatus(fixture('tailscale_status_running.json'));
      expect(s.onlineNames, containsAll(['thistle-46cf20', 'homelab', 'Pixel 8', 'pixel-8']));
      expect(s.onlineNames, isNot(contains('Someone’s MacBook Pro')));
    });

    test('stopped: no address, and a reason to show', () {
      final s = parseTailscaleStatus(fixture('tailscale_status_stopped.json'));
      expect(s.state, TailnetState.stopped);
      expect(s.address, isNull);
      expect(s.peers, isEmpty);
      expect(s.problem, contains('Tailscale is stopped'));
    });

    test('needs a sign-in: says so, with the URL', () {
      final s = parseTailscaleStatus(fixture('tailscale_status_needs_login.json'));
      expect(s.state, TailnetState.needsLogin);
      expect(s.address, isNull);
      expect(s.problem, contains('https://login.tailscale.com/a/0123456789abcd'));
    });

    test('not JSON: not running', () {
      final s = parseTailscaleStatus('failed to connect to local tailscaled; it doesn\'t appear to be running');
      expect(s.state, TailnetState.notRunning);
      expect(s.problem, contains('Tailscale is not running'));
    });

    test('running has no problem', () {
      expect(parseTailscaleStatus(fixture('tailscale_status_running.json')).problem, isNull);
    });
  });

  group('readTailscaleStatus', () {
    test('no tailscale command: not installed', () async {
      final s = await readTailscaleStatus(run: (exe, args) async => throw const ProcessException('tailscale', []));
      expect(s.state, TailnetState.notInstalled);
      expect(s.problem, contains('not installed'));
    });

    test('the daemon not answering: not running', () async {
      final s = await readTailscaleStatus(
        run: (exe, args) async => ProcessResult(1, 1, '', 'failed to connect to local tailscaled'),
      );
      expect(s.state, TailnetState.notRunning);
    });

    test('asks for the JSON status', () async {
      List<String>? asked;
      final s = await readTailscaleStatus(
        run: (exe, args) async {
          asked = [exe, ...args];
          return ProcessResult(1, 0, fixture('tailscale_status_running.json'), '');
        },
      );
      expect(asked, ['tailscale', 'status', '--json']);
      expect(s.state, TailnetState.running);
    });

    test('macOS: no tailscale on the PATH, the Tailscale app\'s own binary', () async {
      final tried = <String>[];
      final s = await readTailscaleStatus(
        commands: tailscaleCommands(macOS: true),
        run: (exe, args) async {
          tried.add(exe);
          if (!exe.startsWith('/Applications/')) throw ProcessException(exe, args);
          return ProcessResult(1, 0, fixture('tailscale_status_running.json'), '');
        },
      );
      expect(tried, [
        'tailscale',
        '/opt/homebrew/bin/tailscale',
        '/usr/local/bin/tailscale',
        '/Applications/Tailscale.app/Contents/MacOS/Tailscale',
      ]);
      expect(s.state, TailnetState.running);
    });
  });

  test('what is shared with a contact that asks: the online peers by MagicDNS name; Android shares none', () {
    final s = parseTailscaleStatus(fixture('tailscale_status_running.json'));
    expect(s.toShare, const [
      SharedPeer('thistle-46cf20', ['100.110.0.38', 'fd7a:115c:a1e0::bb02']),
      SharedPeer('homelab', ['100.125.0.43', 'fd7a:115c:a1e0::dd04']),
      SharedPeer('pixel-8', ['fd7a:115c:a1e0::ee05']),
    ]);
    expect(
      statusFromInterfaces({
        'tun0': ['100.99.0.7'],
      }).toShare,
      isEmpty,
    );
  });

  group('discoverThroughContacts', () {
    final laptop = Contact(id: 1, name: 'laptop', key: 'k1', address: const PeerAddress('100.64.0.4', 4810));
    final device = Contact(id: 2, name: 'device', key: 'k2', address: const PeerAddress('100.64.0.5', 4810));

    test("asks each contact, probes each tailnet address shared once, and says who shared it", () async {
      final asked = <String>[];
      final probed = <PeerAddress>[];
      final found = await discoverThroughContacts(
        [laptop, device],
        (c) async {
          asked.add(c.name);
          if (c.name == 'device') return null; // announces no `peers`
          return const [
            SharedPeer('homelab', ['100.64.0.2', 'fd7a:115c:a1e0::2']),
            SharedPeer('evil', ['8.8.8.8', '127.0.0.1']),
            SharedPeer('me', ['100.99.0.7']),
            SharedPeer('device', ['100.64.0.5']),
            SharedPeer('nothing', ['100.64.0.9']),
          ];
        },
        (address) async {
          probed.add(address);
          return address.host == '100.64.0.2'
              ? ProbeResult(address, 'ab' * 32, 'homelab', 'abcd abcd abcd abcd abcd', capsV1)
              : null;
        },
        ownAddresses: {'100.99.0.7'},
      );
      expect(asked.toSet(), {'laptop', 'device'});
      expect(probed.toSet(), {
        const PeerAddress('100.64.0.2', 4810),
        const PeerAddress('fd7a:115c:a1e0::2', 4810),
        const PeerAddress('100.64.0.9', 4810),
      });
      expect(found.single.probe.name, 'homelab');
      expect(found.single.via, 'laptop');
      expect(found.single.peer.hostName, '', reason: 'no name is taken from the shared list');
    });

    test('a contact that cannot be asked is passed over', () async {
      final found = await discoverThroughContacts(
        [laptop],
        (c) async => throw const SocketException('down'),
        (a) async => null,
      );
      expect(found, isEmpty);
    });
  });

  group('discoverChatPeers', () {
    test('lists the online peers that answer as chat peers, with their tailnet names', () async {
      final s = parseTailscaleStatus(fixture('tailscale_status_running.json'));
      final probed = <PeerAddress>[];
      final found = await discoverChatPeers(s, (address) async {
        probed.add(address);
        if (address.host == '100.110.0.38') {
          return ProbeResult(address, 'ab' * 32, 'thistle-46cf20', 'abcd abcd abcd abcd abcd', capsV1);
        }
        return null;
      });
      expect(probed.toSet(), {
        const PeerAddress('100.110.0.38', 4810),
        const PeerAddress('100.125.0.43', 4810),
        const PeerAddress('fd7a:115c:a1e0::ee05', 4810),
      });
      expect(found, hasLength(1));
      expect(found.single.peer.hostName, 'thistle-46cf20');
      expect(found.single.probe.key, 'ab' * 32);
    });

    test('a stopped tailnet probes nothing', () async {
      final s = parseTailscaleStatus(fixture('tailscale_status_stopped.json'));
      var probes = 0;
      final found = await discoverChatPeers(s, (a) async {
        probes++;
        return null;
      });
      expect(found, isEmpty);
      expect(probes, 0);
    });
  });
}
