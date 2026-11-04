import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/call_engine.dart';

void main() {
  test("a phone's front camera comes first, so a call starts on it; Switch camera goes to the back one", () {
    // flutter_webrtc's labels on Android: a Pixel lists its back camera first.
    expect(
      frontFirst([('0', 'Camera 0, Facing back, Orientation 90'), ('1', 'Camera 1, Facing front, Orientation 270')]),
      ['1', '0'],
    );
  });

  test("cameras that say nothing of facing keep the order they came in (Linux's)", () {
    expect(frontFirst([('/dev/video2', 'USB Camera'), ('/dev/video0', 'Integrated_Webcam_HD')]), [
      '/dev/video2',
      '/dev/video0',
    ]);
  });

  test('only a camera facing front is mirrored', () {
    expect(mirrored('Camera 1, Facing front, Orientation 270'), isTrue);
    expect(mirrored('Camera 0, Facing back, Orientation 90'), isFalse);
    expect(mirrored('Integrated_Webcam_HD'), isTrue, reason: "a laptop's camera faces its user");
  });
}
