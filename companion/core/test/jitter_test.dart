import 'dart:typed_data';

import 'package:tailnet_chat_core/tailnet_chat_core.dart';
import 'package:test/test.dart';

Uint8List p(int seq) => Uint8List.fromList([seq & 0xff]);

/// What `n` pulls (20 ms each) give: the seq played, 'x' concealed, '.' waiting.
List<Object> pulls(JitterBuffer b, int n) => [
  for (var i = 0; i < n; i++)
    switch (b.pull()) {
      Play(:final packet) => packet[0],
      Conceal() => 'x',
      Wait() => '.',
    },
];

void main() {
  test('holds 60 ms of audio before it plays', () {
    final b = JitterBuffer();
    b.push(0, p(0));
    b.push(1, p(1));
    expect(pulls(b, 1), ['.']);
    b.push(2, p(2));
    expect(pulls(b, 3), [0, 1, 2]);
  });

  test('plays packets that arrive out of order in sequence', () {
    final b = JitterBuffer();
    for (final s in [1, 0, 3, 2, 4]) {
      b.push(s, p(s));
    }
    expect(pulls(b, 5), [0, 1, 2, 3, 4]);
  });

  test('conceals a lost packet once and goes on with the next', () {
    final b = JitterBuffer();
    for (final s in [0, 1, 2, 4, 5]) {
      b.push(s, p(s));
    }
    expect(pulls(b, 5), [0, 1, 2, 'x', 4]);
  });

  test('drops a packet that arrives after its turn', () {
    final b = JitterBuffer();
    for (final s in [0, 1, 2, 4, 5, 6]) {
      b.push(s, p(s));
    }
    expect(pulls(b, 4), [0, 1, 2, 'x']);
    b.push(3, p(3));
    expect(pulls(b, 2), [4, 5]);
    expect(b.late, 1);
  });

  test('a duplicate is played once', () {
    final b = JitterBuffer();
    for (final s in [0, 1, 1, 2]) {
      b.push(s, p(s));
    }
    expect(pulls(b, 5), [0, 1, 2, 'x', '.']);
  });

  test('after running dry it fills again, holding a little more each time, never past 200 ms', () {
    final b = JitterBuffer();
    expect(b.delayMs, 60);
    var seq = 0;
    for (var round = 0; round < 12; round++) {
      while (b.pull() is Wait) {
        b.push(seq, p(seq));
        seq++;
      }
      while (b.pull() is Play) {}
    }
    expect(b.delayMs, 200);
  });

  test('more than 200 ms queued is cut back to the target, oldest first', () {
    final b = JitterBuffer();
    for (var s = 0; s < 11; s++) {
      b.push(s, p(s));
    }
    expect(b.queuedMs, 60);
    expect(pulls(b, 3), [8, 9, 10]);
  });

  test('a sender that starts counting again is followed', () {
    final b = JitterBuffer();
    for (final s in [500, 501, 502]) {
      b.push(s, p(s));
    }
    expect(pulls(b, 3), [500 & 0xff, 501 & 0xff, 502 & 0xff]);
    for (final s in [0, 1, 2]) {
      b.push(s, p(s));
    }
    expect(pulls(b, 3), [0, 1, 2]);
  });
}
