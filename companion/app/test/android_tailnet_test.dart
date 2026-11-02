import 'dart:io';

import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/tailnet.dart';

void main() {
  group('statusFromInterfaces (Android: the Tailscale app as a VPN)', () {
    test('a tun interface with a 100.64.0.0/10 address: running, listening there', () {
      final s = statusFromInterfaces({
        'wlan0': ['192.168.1.23', 'fe80::1'],
        'tun0': ['100.101.102.103', 'fd7a:115c:a1e0::1'],
      });
      expect(s.state, TailnetState.running);
      expect(s.address, '100.101.102.103');
      expect(s.problem, isNull);
    });

    test('Android has no peer list: no peers, and discovery is not offered', () {
      final s = statusFromInterfaces({
        'tun0': ['100.64.0.1'],
      });
      expect(s.peers, isEmpty);
      expect(s.hasPeerList, isFalse);
      expect(parseTailscaleStatus('{"BackendState":"Running"}').hasPeerList, isTrue);
    });

    test('the edges of 100.64.0.0/10', () {
      expect(
        statusFromInterfaces({
          'tun0': ['100.127.255.254'],
        }).state,
        TailnetState.running,
      );
      expect(
        statusFromInterfaces({
          'tun0': ['100.63.255.255'],
        }).state,
        TailnetState.appNotConnected,
      );
      expect(
        statusFromInterfaces({
          'tun0': ['100.128.0.1'],
        }).state,
        TailnetState.appNotConnected,
      );
    });

    test('no tailnet address (WiFi or mobile data only): the Tailscale app is not connected', () {
      final s = statusFromInterfaces({
        'wlan0': ['192.168.1.23'],
        'rmnet0': ['10.11.12.13'],
      });
      expect(s.state, TailnetState.appNotConnected);
      expect(s.address, isNull);
      expect(
        s.problem,
        'The Tailscale app is not connected, so nothing can reach this phone. Open Tailscale and connect.',
      );
    });

    test('a carrier NAT address on the mobile interface is not the tailnet', () {
      // 100.64.0.0/10 is also carrier-grade NAT space: only a VPN interface counts.
      final s = statusFromInterfaces({
        'rmnet_data0': ['100.70.1.2'],
      });
      expect(s.state, TailnetState.appNotConnected);
    });

    test('readAndroidTailnet lists the interfaces', () async {
      final s = await readAndroidTailnet(
        list: () async => {
          'tun0': ['100.99.0.7'],
        },
      );
      expect(s.address, '100.99.0.7');
    });

    test('readAndroidTailnet: a failure to list is not connected', () async {
      final s = await readAndroidTailnet(list: () async => throw const SocketException('no'));
      expect(s.state, TailnetState.appNotConnected);
    });
  });
}
