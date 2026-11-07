import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/call_engine.dart';

void main() {
  // flutter_webrtc's labels on Android; a Pixel lists its back camera first.
  final back = Camera.labelled('0', 'Camera 0, Facing back, Orientation 90');
  final front = Camera.labelled('1', 'Camera 1, Facing front, Orientation 270');
  final laptop = Camera.labelled('/dev/video0', 'Integrated_Webcam_HD');

  test('a camera faces the way its label says, or no way said', () {
    expect((back.facing, front.facing, laptop.facing), (Facing.back, Facing.front, null));
  });

  test("a phone's front camera comes first, so a call starts on it; Switch camera goes to the back one", () {
    expect(frontFirst([back, front]).map((c) => c.id), ['1', '0']);
  });

  test("cameras that say nothing of facing keep the order they came in (Linux's)", () {
    expect(frontFirst([const Camera('/dev/video2'), laptop]).map((c) => c.id), ['/dev/video2', '/dev/video0']);
  });

  test('only a camera facing front, or saying nothing of facing, is mirrored', () {
    expect(front.mirrored, isTrue);
    expect(back.mirrored, isFalse);
    expect(laptop.mirrored, isTrue, reason: "a laptop's camera faces its user");
  });
}
