import 'dart:async';

import 'package:flutter_test/flutter_test.dart';
import 'package:thistle_chat/src/child_processes.dart';

void main() {
  test('no child starts while a camera is on, and one may again once it is off', () async {
    final children = ChildProcesses();
    var started = 0;
    Future<int> child() async => ++started;
    expect(await children.run(child), 1);
    await children.cameraOpening();
    expect(await children.run(child), isNull);
    expect(children.mayStart, isFalse);
    expect(started, 1);
    children.cameraClosed();
    expect(await children.run(child), 2);
    expect(children.mayStart, isTrue);
  });

  test('a camera opens only once the children already running have exited', () async {
    final children = ChildProcesses();
    final exit = Completer<void>();
    final running = children.run(() => exit.future);
    var opened = false;
    final opening = children.cameraOpening().then((_) => opened = true);
    await Future<void>.delayed(const Duration(milliseconds: 50));
    expect(opened, isFalse);
    expect(await children.run(() async => 1), isNull, reason: 'none starts meanwhile');
    exit.complete();
    await running;
    await opening;
    expect(opened, isTrue);
  });

  test('a child that does not exit holds the camera back only so long', () async {
    final children = ChildProcesses();
    unawaited(children.run(() => Completer<void>().future));
    final clock = Stopwatch()..start();
    await children.cameraOpening(wait: const Duration(milliseconds: 100));
    expect(clock.elapsedMilliseconds, lessThan(1000));
    expect(children.cameraOn, isTrue);
  });
}
