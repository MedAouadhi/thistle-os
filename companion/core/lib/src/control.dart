/// The control protocol (docs/tailnet-chat/README.md): one JSON command a
/// line, by which the conformance suite drives the core as it drives the
/// reference chat peer.
library;

import 'dart:io';

import 'peer.dart';
import 'policy.dart';
import 'store.dart';

Map<String, Object?> identityJson(ChatPeer peer) => {
  'key': peer.identity.keyHex,
  'fingerprint': peer.identity.fingerprint,
  'name': peer.name,
  'address': [peer.address.host, peer.address.port],
};

/// `to` as the control protocol gives it: a key, a contact's name, or
/// `[host, port]`.
Object _who(Object? to) {
  if (to is List) return PeerAddress(to[0] as String, to[1] as int);
  if (to is String) return to;
  throw ArgumentError('no peer named $to');
}

Map<String, Object?> _contactJson(Contact c) => {
  'name': c.name,
  'key': c.key,
  'address': c.address == null ? null : [c.address!.host, c.address!.port],
  'tailnet_name': c.tailnetName,
  'flagged_key': c.flaggedKey,
  'accepted_by_them': c.acceptedByThem,
  'declined_by_them': c.declinedByThem,
};

/// One control command; its result, JSON-able.
Future<Object?> control(ChatPeer peer, Map<String, dynamic> command) async {
  final to = command['to'];
  switch (command['cmd']) {
    case 'identity':
      return identityJson(peer);
    case 'add_contact':
      final a = command['address'] as List?;
      final c = peer.addContact(
        a == null ? null : PeerAddress(a[0] as String, a[1] as int),
        name: command['name'] as String?,
        key: command['key'] as String?,
      );
      return {'name': c.name};
    case 'contacts':
      return peer.contacts().map(_contactJson).toList();
    case 'requests':
      return [
        for (final r in peer.requests())
          {
            'key': r.key,
            'name': r.name,
            'fingerprint': r.fingerprint,
            'held': r.held.where((m) => m.kind != 'react').map(messageJson).toList(),
          },
      ];
    case 'accept':
      peer.accept(command['key'] as String);
      return true;
    case 'decline':
      peer.decline(command['key'] as String);
      return true;
    case 'accept_new_key':
      peer.acceptNewKey(command['contact'] as String);
      return true;
    case 'rename':
      peer.renameContact(_who(to ?? command['contact']), command['name'] as String);
      return true;
    case 'remove_contact':
      peer.removeContact(_who(to ?? command['contact']));
      return true;
    case 'send_text':
      return peer.sendText(_who(to), command['text'] as String, replyTo: command['reply_to'] as String?);
    case 'react':
      return peer.react(_who(to), command['message'] as String, command['emoji'] as String);
    case 'send_voice':
      return peer.sendVoice(_who(to), File(command['file'] as String).readAsBytesSync(), command['ms'] as int);
    case 'mark_read':
      peer.markRead(_who(to), ids: (command['ids'] as List?)?.cast<String>());
      return true;
    case 'typing':
      peer.setTyping(_who(to), command['on'] as bool);
      return true;
    case 'set':
      if (command.containsKey('read_receipts')) peer.readReceipts = command['read_receipts'] as bool;
      if (command.containsKey('call_policy'))
        peer.callPolicy = CallPolicy.values.byName(command['call_policy'] as String);
      return true;
    case 'messages':
      return peer.messages(_who(to)).map(messageJson).toList();
    case 'waiting':
      return peer.waiting(_who(to));
    case 'is_typing':
      return peer.isTyping(command['key'] as String);
    case 'flush':
      await peer.flush(to == null ? null : _who(to));
      return true;
    case 'call':
      return peer.call(_who(to));
    case 'calls':
      return {
        for (final MapEntry(:key, :value) in peer.calls.entries)
          key: {'peer': value.peer, 'state': value.state, 'frames': value.frames},
      };
    case 'end_call':
      peer.endCall(command['call'] as String, reason: (command['reason'] as String?) ?? 'hangup');
      return true;
    case 'stop':
      return true; // the caller stops the peer after answering
    default:
      throw ArgumentError('no command ${command['cmd']}');
  }
}
