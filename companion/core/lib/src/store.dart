/// The companion's local database: contacts, contact requests,
/// conversations, reactions, the outbox and settings, in SQLite.
///
/// A conversation, and an outbox, is a "box": the contact's pinned key in
/// hex, or `contact:<id>` for a contact added by address whose key is not
/// pinned yet. Pinning renames the box.
library;

import 'dart:convert';
import 'dart:typed_data';

import 'package:sqlite3/sqlite3.dart';

import 'policy.dart' show PeerAddress;

const int schemaVersion = 2;

const String _schema = '''
CREATE TABLE IF NOT EXISTS contacts (
  id INTEGER PRIMARY KEY AUTOINCREMENT,
  name TEXT NOT NULL,
  key TEXT UNIQUE,
  host TEXT,
  port INTEGER,
  tailnet_name TEXT,
  flagged_key TEXT,
  accepted_by_them INTEGER,
  declined_by_them INTEGER NOT NULL DEFAULT 0,
  webrtc INTEGER NOT NULL DEFAULT 0
);
CREATE TABLE IF NOT EXISTS requests (
  key TEXT PRIMARY KEY,
  name TEXT NOT NULL,
  host TEXT,
  fingerprint TEXT NOT NULL,
  first_seen INTEGER NOT NULL
);
CREATE TABLE IF NOT EXISTS messages (
  box TEXT NOT NULL,
  id TEXT NOT NULL,
  mine INTEGER NOT NULL,
  ts INTEGER NOT NULL,
  kind TEXT NOT NULL,
  text TEXT,
  reply_to TEXT,
  voice BLOB,
  ms INTEGER,
  react_to TEXT,
  emoji TEXT,
  state TEXT NOT NULL,
  held INTEGER NOT NULL DEFAULT 0,
  PRIMARY KEY (box, id, mine)
);
CREATE INDEX IF NOT EXISTS messages_by_box ON messages (box, held, ts);
CREATE TABLE IF NOT EXISTS reactions (
  box TEXT NOT NULL,
  message TEXT NOT NULL,
  who TEXT NOT NULL,
  emoji TEXT NOT NULL,
  ts INTEGER NOT NULL,
  PRIMARY KEY (box, message, who)
);
CREATE TABLE IF NOT EXISTS outbox (
  seq INTEGER PRIMARY KEY AUTOINCREMENT,
  box TEXT NOT NULL,
  id TEXT NOT NULL,
  frame TEXT NOT NULL,
  data BLOB
);
CREATE TABLE IF NOT EXISTS settings (
  name TEXT PRIMARY KEY,
  value TEXT NOT NULL
);
''';

class Contact {
  Contact({
    required this.id,
    required this.name,
    this.key,
    this.address,
    this.tailnetName,
    this.flaggedKey,
    this.acceptedByThem,
    this.declinedByThem = false,
    this.webrtc = false,
  });
  final int id;
  final String name;
  final String? key;
  final PeerAddress? address;
  final String? tailnetName;
  final String? flaggedKey;
  final bool? acceptedByThem;
  final bool declinedByThem;

  /// Whether its last hello announced `call:webrtc`: video-capable, if ours
  /// does too, until a hello says otherwise.
  final bool webrtc;

  String get box => key ?? 'contact:$id';
}

class ContactRequest {
  ContactRequest({required this.key, required this.name, this.host, required this.fingerprint, required this.held});
  final String key;
  final String name;
  final String? host;
  final String fingerprint;
  final List<Message> held;
}

/// A text or voice message, or a received reaction (kept so a duplicate
/// is acked and not applied twice; never shown as a message).
class Message {
  Message({
    required this.id,
    required this.mine,
    required this.ts,
    required this.kind,
    required this.state,
    this.text,
    this.replyTo,
    this.voice,
    this.ms,
    this.reactTo,
    this.emoji,
    this.reactions = const {},
  });
  final String id;
  final bool mine;
  final int ts;

  /// `text`, `voice` or `react`.
  final String kind;

  /// Mine: `waiting`, `delivered`, `read`. Theirs: `received`, `read`.
  final String state;
  final String? text;
  final String? replyTo;
  final Uint8List? voice;
  final int? ms;
  final String? reactTo;
  final String? emoji;

  /// `me` and `them` -> emoji.
  final Map<String, String> reactions;
}

class OutboxEntry {
  OutboxEntry(this.id, this.frame, this.data);
  final String id;
  final Map<String, dynamic> frame;
  final Uint8List? data;
}

class Store {
  Store._(this.db) {
    final was = db.select('PRAGMA user_version').first.values.first as int;
    db.execute(_schema);
    // Version 1 knew nothing of a contact's call:webrtc.
    if (was == 1) db.execute('ALTER TABLE contacts ADD COLUMN webrtc INTEGER NOT NULL DEFAULT 0');
    db.execute('PRAGMA user_version = $schemaVersion');
  }

  /// Open (or create) the database at `path`; `:memory:` keeps nothing.
  factory Store.open(String path) {
    final db = path == ':memory:' ? sqlite3.openInMemory() : sqlite3.open(path);
    db.execute('PRAGMA journal_mode = WAL');
    db.execute('PRAGMA foreign_keys = ON');
    return Store._(db);
  }

  final Database db;

  void close() => db.close();

  T transaction<T>(T Function() body) {
    db.execute('BEGIN');
    try {
      final out = body();
      db.execute('COMMIT');
      return out;
    } catch (_) {
      db.execute('ROLLBACK');
      rethrow;
    }
  }

  // -- settings -----------------------------------------------------------------

  String? setting(String name) =>
      db.select('SELECT value FROM settings WHERE name = ?', [name]).firstOrNull?['value'] as String?;

  void setSetting(String name, String value) => db.execute(
    'INSERT INTO settings (name, value) VALUES (?, ?) '
    'ON CONFLICT(name) DO UPDATE SET value = excluded.value',
    [name, value],
  );

  // -- contacts -----------------------------------------------------------------

  Contact _contact(Row r) => Contact(
    id: r['id'] as int,
    name: r['name'] as String,
    key: r['key'] as String?,
    address: r['host'] == null ? null : PeerAddress(r['host'] as String, r['port'] as int),
    tailnetName: r['tailnet_name'] as String?,
    flaggedKey: r['flagged_key'] as String?,
    acceptedByThem: r['accepted_by_them'] == null ? null : r['accepted_by_them'] == 1,
    declinedByThem: r['declined_by_them'] == 1,
    webrtc: r['webrtc'] == 1,
  );

  List<Contact> contacts() => db.select('SELECT * FROM contacts ORDER BY id').map(_contact).toList();

  Contact? contactById(int id) => db.select('SELECT * FROM contacts WHERE id = ?', [id]).map(_contact).firstOrNull;

  Contact? contactByKey(String key) =>
      db.select('SELECT * FROM contacts WHERE key = ?', [key]).map(_contact).firstOrNull;

  Contact addContact({required String name, String? key, PeerAddress? address, String? tailnetName}) {
    db.execute('INSERT INTO contacts (name, key, host, port, tailnet_name) VALUES (?, ?, ?, ?, ?)', [
      name,
      key,
      address?.host,
      address?.port,
      tailnetName,
    ]);
    return contactById(db.lastInsertRowId)!;
  }

  void renameContact(int id, String name) => db.execute('UPDATE contacts SET name = ? WHERE id = ?', [name, id]);

  void setTailnetName(int id, String? name) =>
      db.execute('UPDATE contacts SET tailnet_name = ? WHERE id = ?', [name, id]);

  /// What the contact's last hello said of `call:webrtc`.
  void setWebrtc(int id, bool on) => db.execute('UPDATE contacts SET webrtc = ? WHERE id = ?', [on ? 1 : 0, id]);

  void flagKey(int id, String key) => db.execute('UPDATE contacts SET flagged_key = ? WHERE id = ?', [key, id]);

  void setAnswer(int id, {required bool accepted}) => db.execute(
    'UPDATE contacts SET accepted_by_them = ?, declined_by_them = ? WHERE id = ?',
    [accepted ? 1 : 0, accepted ? 0 : 1, id],
  );

  /// Pin `key` for a contact (first handshake, or a new key accepted), and
  /// move its conversation and outbox to the key's box.
  void pinKey(int id, String key) => transaction(() {
    final c = contactById(id)!;
    final old = c.box;
    db.execute('UPDATE contacts SET key = ?, flagged_key = NULL WHERE id = ?', [key, id]);
    _renameBox(old, key);
  });

  void _renameBox(String from, String to) {
    if (from == to) return;
    db.execute('UPDATE OR IGNORE messages SET box = ? WHERE box = ?', [to, from]);
    db.execute('UPDATE OR IGNORE reactions SET box = ? WHERE box = ?', [to, from]);
    db.execute('UPDATE outbox SET box = ? WHERE box = ?', [to, from]);
  }

  /// Unpin a contact: what it sends next is a contact request again.
  void removeContact(int id) => transaction(() {
    final c = contactById(id);
    if (c == null) return;
    db.execute('DELETE FROM outbox WHERE box = ?', [c.box]);
    db.execute('DELETE FROM contacts WHERE id = ?', [id]);
  });

  // -- requests -----------------------------------------------------------------

  ContactRequest? request(String key) {
    final r = db.select('SELECT * FROM requests WHERE key = ?', [key]).firstOrNull;
    if (r == null) return null;
    return ContactRequest(
      key: key,
      name: r['name'] as String,
      host: r['host'] as String?,
      fingerprint: r['fingerprint'] as String,
      held: _messages(key, held: true),
    );
  }

  List<ContactRequest> requests() => [
    for (final r in db.select('SELECT key FROM requests ORDER BY first_seen, key')) request(r['key'] as String)!,
  ];

  void addRequest({
    required String key,
    required String name,
    String? host,
    required String fingerprint,
    required int now,
  }) => db.execute('INSERT OR IGNORE INTO requests (key, name, host, fingerprint, first_seen) VALUES (?, ?, ?, ?, ?)', [
    key,
    name,
    host,
    fingerprint,
    now,
  ]);

  /// Accept a request: a contact with its key pinned, its held messages
  /// shown (and its held reactions applied).
  Contact acceptRequest(String key) => transaction(() {
    final req = request(key)!;
    final c = contactByKey(key) ?? addContact(name: req.name, key: key, tailnetName: req.name);
    db.execute('DELETE FROM requests WHERE key = ?', [key]);
    db.execute('UPDATE messages SET held = 0 WHERE box = ?', [key]);
    for (final m in req.held.where((m) => m.kind == 'react')) {
      applyReaction(key, m.reactTo!, 'them', m.emoji!, m.ts);
    }
    return c;
  });

  /// Decline a request: what it held is dropped.
  void declineRequest(String key) => transaction(() {
    db.execute('DELETE FROM requests WHERE key = ?', [key]);
    db.execute('DELETE FROM messages WHERE box = ? AND held = 1', [key]);
  });

  // -- messages -----------------------------------------------------------------

  Message _message(Row r, Map<String, Map<String, String>> reactions) => Message(
    id: r['id'] as String,
    mine: r['mine'] == 1,
    ts: r['ts'] as int,
    kind: r['kind'] as String,
    state: r['state'] as String,
    text: r['text'] as String?,
    replyTo: r['reply_to'] as String?,
    voice: r['voice'] as Uint8List?,
    ms: r['ms'] as int?,
    reactTo: r['react_to'] as String?,
    emoji: r['emoji'] as String?,
    reactions: reactions[r['id']] ?? const {},
  );

  List<Message> _messages(String box, {required bool held, bool withReactions = true}) {
    final reactions = <String, Map<String, String>>{};
    for (final r in db.select('SELECT message, who, emoji FROM reactions WHERE box = ?', [box])) {
      (reactions[r['message'] as String] ??= {})[r['who'] as String] = r['emoji'] as String;
    }
    return db
        .select('SELECT * FROM messages WHERE box = ? AND held = ? ORDER BY ts, rowid', [box, held ? 1 : 0])
        .where((r) => withReactions || r['kind'] != 'react')
        .map((r) => _message(r, reactions))
        .toList();
  }

  /// A conversation in the order written: text and voice messages.
  List<Message> conversation(String box) => _messages(box, held: false).where((m) => m.kind != 'react').toList();

  Message? message(String box, String id, {required bool mine}) {
    final r = db.select('SELECT * FROM messages WHERE box = ? AND id = ? AND mine = ?', [box, id, mine ? 1 : 0]);
    return r.isEmpty ? null : _message(r.first, const {});
  }

  /// Store a received message, reaction or voice message once; false if its
  /// id was already stored.
  bool storeReceived(String box, Message m, {required bool held}) {
    db.execute(
      'INSERT OR IGNORE INTO messages (box, id, mine, ts, kind, text, reply_to, voice, ms, react_to, emoji, state, held) '
      'VALUES (?, ?, 0, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)',
      [box, m.id, m.ts, m.kind, m.text, m.replyTo, m.voice, m.ms, m.reactTo, m.emoji, m.state, held ? 1 : 0],
    );
    return db.updatedRows > 0;
  }

  /// One of mine, with its frame queued in the outbox, at once.
  void storeMineAndQueue(String box, Message m, Map<String, Object?> frame, {Uint8List? data}) => transaction(() {
    if (m.kind != 'react') {
      db.execute(
        'INSERT INTO messages (box, id, mine, ts, kind, text, reply_to, voice, ms, state) '
        'VALUES (?, ?, 1, ?, ?, ?, ?, ?, ?, ?)',
        [box, m.id, m.ts, m.kind, m.text, m.replyTo, m.voice, m.ms, m.state],
      );
    }
    queue(box, frame, data: data);
  });

  void setMineState(String box, String id, String state, {List<String> from = const ['waiting']}) => db.execute(
    'UPDATE messages SET state = ? WHERE box = ? AND id = ? AND mine = 1 AND state IN (${from.map((_) => '?').join(', ')})',
    [state, box, id, ...from],
  );

  /// Mark received messages read; the ids that were unread.
  List<String> markRead(String box, {List<String>? ids}) => transaction(() {
    final unread = [
      for (final r in db.select(
        "SELECT id FROM messages WHERE box = ? AND mine = 0 AND held = 0 AND kind != 'react' AND state = 'received' "
        'ORDER BY ts, rowid',
        [box],
      ))
        r['id'] as String,
    ].where((id) => ids == null || ids.contains(id)).toList();
    for (final id in unread) {
      db.execute("UPDATE messages SET state = 'read' WHERE box = ? AND id = ? AND mine = 0", [box, id]);
    }
    return unread;
  });

  /// One reaction per sender per message: a later one replaces it, an empty
  /// emoji removes it.
  void applyReaction(String box, String message, String who, String emoji, int ts) {
    final existing =
        db.select('SELECT ts FROM reactions WHERE box = ? AND message = ? AND who = ?', [
              box,
              message,
              who,
            ]).firstOrNull?['ts']
            as int?;
    if (existing != null && ts < existing) return;
    if (emoji.isEmpty) {
      db.execute('DELETE FROM reactions WHERE box = ? AND message = ? AND who = ?', [box, message, who]);
    } else {
      db.execute(
        'INSERT INTO reactions (box, message, who, emoji, ts) VALUES (?, ?, ?, ?, ?) '
        'ON CONFLICT(box, message, who) DO UPDATE SET emoji = excluded.emoji, ts = excluded.ts',
        [box, message, who, emoji, ts],
      );
    }
  }

  // -- the outbox ---------------------------------------------------------------

  void queue(String box, Map<String, Object?> frame, {Uint8List? data}) => db.execute(
    'INSERT INTO outbox (box, id, frame, data) VALUES (?, ?, ?, ?)',
    [box, frame['id'], jsonEncode(frame), data],
  );

  List<OutboxEntry> outbox(String box) => [
    for (final r in db.select('SELECT id, frame, data FROM outbox WHERE box = ? ORDER BY seq', [box]))
      OutboxEntry(r['id'] as String, jsonDecode(r['frame'] as String) as Map<String, dynamic>, r['data'] as Uint8List?),
  ];

  /// Every box with something waiting.
  List<String> waitingBoxes() => [
    for (final r in db.select('SELECT DISTINCT box FROM outbox ORDER BY box')) r['box'] as String,
  ];

  /// An ack: the frame leaves the outbox; true if it was there.
  bool acked(String box, String id) {
    db.execute('DELETE FROM outbox WHERE box = ? AND id = ?', [box, id]);
    return db.updatedRows > 0;
  }

  void dropOutbox(String box) => db.execute('DELETE FROM outbox WHERE box = ?', [box]);
}
