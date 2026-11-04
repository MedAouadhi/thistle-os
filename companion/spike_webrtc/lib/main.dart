// Throwaway spike for ticket 02 of .scratch/tailnet-video: a WebRTC call
// with tailnet candidates only, VP8 preferred, and measurements for
// flutter_webrtc issues 2070 (UI blocked on calls after the first) and
// 2225 (the camera kept busy by a child process).
//
// Modes, by --dart-define=MODE=...:
//   loop  (default) two peer connections in this one process, calling each
//         other over this machine's tailnet address; ROUNDS calls in a row.
//   offer / answer  one side each, for a call between two machines (or a
//         machine and a phone): the SDP and candidates go through a small
//         TCP signalling socket, SIGNAL=host:port (answer listens, offer
//         connects). The procedure is in
//         .scratch/tailnet-video/issues/02-spike-webrtc-over-the-tailnet.md.
//
// Everything is printed (stdout on Linux, logcat on Android) with a
// "[spike]" prefix.

import 'dart:async';
import 'dart:convert';
import 'dart:io';
import 'dart:ui' show FramePhase;

import 'package:flutter/material.dart';
import 'package:flutter/scheduler.dart';
import 'package:flutter_webrtc/flutter_webrtc.dart';

const mode = String.fromEnvironment('MODE', defaultValue: 'loop');
const rounds = int.fromEnvironment('ROUNDS', defaultValue: 3);
const holdSeconds = int.fromEnvironment('HOLD', defaultValue: 4);
const signal = String.fromEnvironment('SIGNAL', defaultValue: '');
const childTest = bool.fromEnvironment('CHILD_TEST', defaultValue: true);
// Also start a long-lived child (sleep 20) before hanging up, as a microphone
// meter's parecord would be.
const longChildTest = bool.fromEnvironment('LONG_CHILD', defaultValue: true);
const audioOn = bool.fromEnvironment('AUDIO', defaultValue: true);
const videoOn = bool.fromEnvironment('VIDEO', defaultValue: true);

final _t0 = DateTime.now();
int nowMs() => DateTime.now().difference(_t0).inMilliseconds;
// ignore: avoid_print
void log(String s) => print('[spike] ${nowMs().toString().padLeft(6)} $s');

// ---- what blocks the UI -------------------------------------------------

String phase = 'idle';

class Gaps {
  int last = nowMs();
  final List<String> big = [];
  final Map<String, int> worstByPhase = {};
  void tick() {
    final n = nowMs();
    final gap = n - last;
    last = n;
    final w = worstByPhase[phase] ?? 0;
    if (gap > w) worstByPhase[phase] = gap;
    if (gap > 100) {
      big.add('$gap ms timer gap during "$phase"');
      log('TIMER GAP $gap ms during "$phase"');
    }
  }
}

final timerGaps = Gaps();
int lastFrameVsync = 0; // microseconds
final Map<String, int> worstFrameGapByPhase = {};

void onTimings(List<FrameTiming> ts) {
  for (final t in ts) {
    final v = t.timestampInMicroseconds(FramePhase.vsyncStart);
    if (lastFrameVsync != 0) {
      final gapMs = (v - lastFrameVsync) ~/ 1000;
      final w = worstFrameGapByPhase[phase] ?? 0;
      if (gapMs > w) worstFrameGapByPhase[phase] = gapMs;
      if (gapMs > 100) log('FRAME GAP $gapMs ms during "$phase"');
    }
    lastFrameVsync = v;
  }
}

Future<T> timed<T>(String what, Future<T> Function() f) async {
  final prev = phase;
  phase = what;
  final sw = Stopwatch()..start();
  try {
    return await f();
  } finally {
    log('  $what: ${sw.elapsedMilliseconds} ms');
    phase = prev;
  }
}

// ---- candidates ----------------------------------------------------------

bool isTailnetAddress(String a) {
  final v4 = RegExp(r'^(\d+)\.(\d+)\.\d+\.\d+$').firstMatch(a);
  if (v4 != null) {
    final o1 = int.parse(v4.group(1)!), o2 = int.parse(v4.group(2)!);
    return o1 == 100 && o2 >= 64 && o2 <= 127;
  }
  return a.toLowerCase().startsWith('fd7a:115c:a1e0:');
}

/// (address, type, protocol) of an ICE candidate line.
(String, String, String) describe(String cand) {
  final p = cand.split(' ');
  final typIdx = p.indexOf('typ');
  return (p.length > 4 ? p[4] : '?', typIdx >= 0 ? p[typIdx + 1] : '?', p.length > 2 ? p[2] : '?');
}

final Map<String, int> candidateKinds = {};

bool keepCandidate(String who, RTCIceCandidate c) {
  final s = c.candidate ?? '';
  if (s.isEmpty) return false;
  final (addr, typ, proto) = describe(s);
  final keep = isTailnetAddress(addr);
  final k = '$typ/$proto ${keep ? "kept (tailnet)" : "dropped"}';
  candidateKinds[k] = (candidateKinds[k] ?? 0) + 1;
  log('  $who candidate $typ $proto $addr -> ${keep ? "KEEP" : "drop"}');
  return keep;
}

// ---- one side of a call ---------------------------------------------------

const pcConfig = <String, dynamic>{
  'iceServers': <dynamic>[],
  'bundlePolicy': 'max-bundle',
  'rtcpMuxPolicy': 'require',
  'sdpSemantics': 'unified-plan',
  'iceCandidatePoolSize': 0,
};

Future<void> preferVp8(RTCPeerConnection pc) async {
  final caps = await getRtpSenderCapabilities('video');
  final codecs = caps.codecs ?? [];
  final vp8 = codecs.where((c) => c.mimeType.toLowerCase() == 'video/vp8');
  final rest = codecs.where((c) =>
      c.mimeType.toLowerCase() != 'video/vp8' && c.mimeType.toLowerCase() != 'video/h264');
  final order = [...vp8, ...rest];
  for (final t in await pc.getTransceivers()) {
    if (t.sender.track?.kind == 'video' || (await t.getCurrentDirection()) == null) {
      try {
        await t.setCodecPreferences(order);
      } catch (e) {
        log('  setCodecPreferences failed: $e');
      }
    }
  }
}

Future<MediaStream> openCamera() => timed('getUserMedia', () => navigator.mediaDevices.getUserMedia({
      'audio': audioOn,
      'video': videoOn
          ? {
              'width': 640,
              'height': 480,
              'frameRate': 30,
            }
          : false,
    }));

Future<RTCPeerConnection> makePc(String who, MediaStream local,
    void Function(RTCIceCandidate) onCand) async {
  final pc = await timed('$who.createPeerConnection', () => createPeerConnection(pcConfig));
  pc.onIceCandidate = (c) {
    if (keepCandidate(who, c)) onCand(c);
  };
  pc.onIceConnectionState = (s) => log('  $who ice: $s');
  pc.onConnectionState = (s) => log('  $who pc: $s');
  pc.onTrack = (e) => log('  $who got remote ${e.track.kind} track');
  for (final t in local.getTracks()) {
    await timed('$who.addTrack(${t.kind})', () => pc.addTrack(t, local));
  }
  await timed('$who.preferVp8', () => preferVp8(pc));
  return pc;
}

Future<String> statsSummary(RTCPeerConnection pc) async {
  final reports = await pc.getStats();
  final byId = {for (final r in reports) r.id: r};
  final out = <String>[];
  for (final r in reports) {
    final v = r.values;
    if (r.type == 'inbound-rtp' && v['kind'] == 'video') {
      final codec = byId[v['codecId']]?.values['mimeType'];
      out.add('video in: $codec, frames decoded ${v['framesDecoded']}, '
          '${v['frameWidth']}x${v['frameHeight']}');
    }
    if (r.type == 'inbound-rtp' && v['kind'] == 'audio') {
      final codec = byId[v['codecId']]?.values['mimeType'];
      out.add('audio in: $codec, packets ${v['packetsReceived']}');
    }
    if (r.type == 'transport' && v['selectedCandidatePairId'] != null) {
      final pr = byId[v['selectedCandidatePairId']]?.values ?? {};
      final l = byId[pr['localCandidateId']]?.values;
      final rm = byId[pr['remoteCandidateId']]?.values;
      out.add('selected pair: local ${l?['candidateType']} ${l?['address'] ?? l?['ip']}:${l?['port']} '
          '-> remote ${rm?['candidateType']} ${rm?['address'] ?? rm?['ip']}:${rm?['port']} '
          '(${l?['protocol']})');
    }
  }
  return out.join('; ');
}

Future<void> waitConnected(RTCPeerConnection pc, String who) async {
  final sw = Stopwatch()..start();
  while (sw.elapsedMilliseconds < 30000) {
    final s = pc.iceConnectionState;
    if (s == RTCIceConnectionState.RTCIceConnectionStateConnected ||
        s == RTCIceConnectionState.RTCIceConnectionStateCompleted) {
      log('  $who connected after ${sw.elapsedMilliseconds} ms');
      return;
    }
    if (s == RTCIceConnectionState.RTCIceConnectionStateFailed) throw 'ICE failed';
    await Future.delayed(const Duration(milliseconds: 20));
  }
  throw 'not connected after 30 s';
}

// ---- 2225: a child process while the camera is open ------------------------

Future<void> runChild() async {
  final r = await timed('Process.run(tailscale status)',
      () => Process.run('tailscale', ['status'], runInShell: false));
  log('  tailscale status exit ${r.exitCode}, ${(r.stdout as String).split('\n').length} lines');
}

Future<Process> startLongChild() async {
  final p = await Process.start('sleep', ['20']);
  await Future.delayed(const Duration(milliseconds: 200));
  final fds = Directory('/proc/${p.pid}/fd').listSync();
  final video = fds.where((f) {
    try {
      return File(f.path).resolveSymbolicLinksSync().startsWith('/dev/video');
    } catch (_) {
      return false;
    }
  }).map((f) => '${f.path} -> ${File(f.path).resolveSymbolicLinksSync()}');
  log('  child sleep(${p.pid}) holds: ${video.isEmpty ? "no /dev/video*" : video.join(", ")}');
  return p;
}

void ownVideoFdFlags() {
  for (final f in Directory('/proc/self/fd').listSync()) {
    String target;
    try {
      target = File(f.path).resolveSymbolicLinksSync();
    } catch (_) {
      continue;
    }
    if (!target.startsWith('/dev/video')) continue;
    final fd = f.path.split('/').last;
    final info = File('/proc/self/fdinfo/$fd').readAsLinesSync();
    final flags = info.firstWhere((l) => l.startsWith('flags:'), orElse: () => 'flags: ?');
    final oct = int.tryParse(flags.split(':').last.trim(), radix: 8) ?? 0;
    log('  app fd $fd -> $target, $flags, O_CLOEXEC ${(oct & 0x80000) != 0 ? "set" : "NOT set"}');
  }
}

// ---- loop mode: both sides in this process ---------------------------------

Future<void> oneLoopCall(int n, {bool withChild = false}) async {
  log('=== CALL $n: set-up ===');
  final setup = Stopwatch()..start();
  final local = await openCamera();
  if (withChild) {
    ownVideoFdFlags();
    await runChild();
  }
  late RTCPeerConnection a, b;
  final pendingForA = <RTCIceCandidate>[], pendingForB = <RTCIceCandidate>[];
  a = await makePc('A', local, (c) => pendingForB.add(c));
  b = await makePc('B', local, (c) => pendingForA.add(c));
  final offer = await timed('A.createOffer', () => a.createOffer());
  await timed('A.setLocalDescription', () => a.setLocalDescription(offer));
  await timed('B.setRemoteDescription', () => b.setRemoteDescription(offer));
  final answer = await timed('B.createAnswer', () => b.createAnswer());
  await timed('B.setLocalDescription', () => b.setLocalDescription(answer));
  await timed('A.setRemoteDescription', () => a.setRemoteDescription(answer));
  // trickle: flush what has been gathered, keep flushing until connected
  var flushing = true;
  Future<void> flush() async {
    while (flushing) {
      while (pendingForB.isNotEmpty) {
        await b.addCandidate(pendingForB.removeAt(0));
      }
      while (pendingForA.isNotEmpty) {
        await a.addCandidate(pendingForA.removeAt(0));
      }
      await Future.delayed(const Duration(milliseconds: 20));
    }
  }

  final flusher = flush();
  await waitConnected(a, 'A');
  await waitConnected(b, 'B');
  log('CALL $n set-up total ${setup.elapsedMilliseconds} ms');
  phase = 'in call $n';
  await Future.delayed(Duration(seconds: holdSeconds));
  log('  A stats: ${await statsSummary(a)}');
  log('  B stats: ${await statsSummary(b)}');
  final sdpVideo = ((await a.getLocalDescription())?.sdp ?? '')
      .split('\n')
      .firstWhere((l) => l.startsWith('m=video'), orElse: () => '');
  log('  A offer m=video: ${sdpVideo.trim()}');
  flushing = false;
  await flusher;

  Process? longChild;
  if (withChild && longChildTest) {
    longChild = await startLongChild();
  }

  log('=== CALL $n: hang-up ===');
  final down = Stopwatch()..start();
  await timed('A.close', () => a.close());
  await timed('B.close', () => b.close());
  for (final t in local.getTracks()) {
    await timed('track.stop(${t.kind})', () => t.stop());
  }
  await timed('stream.dispose', () => local.dispose());
  await timed('A.dispose', () => a.dispose());
  await timed('B.dispose', () => b.dispose());
  log('CALL $n hang-up total ${down.elapsedMilliseconds} ms');
  phase = 'idle';

  if (withChild) {
    log('=== 2225: a second call after tailscale status'
        '${longChild != null ? ', while sleep(${longChild.pid}) still holds the old camera fd' : ''} ===');
    try {
      await oneLoopCall(n * 10 + 1);
    } catch (e) {
      log('  second call: FAILED: $e');
    }
    if (longChild != null) {
      log('  child still alive: ${Directory('/proc/${longChild.pid}').existsSync()}');
      longChild.kill();
      await longChild.exitCode;
    }
  }
}

Future<void> loopMode() async {
  for (var i = 1; i <= rounds; i++) {
    await oneLoopCall(i);
    await Future.delayed(const Duration(seconds: 1));
  }
  if (childTest) {
    // 2225: tailscale status while the camera is open, then a second call.
    await oneLoopCall(rounds + 1, withChild: true);
    await Future.delayed(const Duration(seconds: 1));
    await oneLoopCall(rounds + 2);
  }
}

// ---- offer / answer mode: one side per process, over a TCP socket ----------

Future<void> pairMode() async {
  final parts = signal.split(':');
  final host = parts.sublist(0, parts.length - 1).join(':');
  final port = int.parse(parts.last);
  late Socket sock;
  if (mode == 'answer') {
    final server = await ServerSocket.bind(InternetAddress.anyIPv4, port);
    log('signalling: listening on $port');
    sock = await server.first;
  } else {
    sock = await Socket.connect(host, port);
  }
  log('signalling: connected to ${sock.remoteAddress.address}');
  final lines = sock.cast<List<int>>().transform(utf8.decoder).transform(const LineSplitter());
  void send(Map<String, dynamic> m) => sock.writeln(jsonEncode(m));
  // One listener for the whole session; frames that arrive between calls
  // wait for the next call's handler. Frames are handled one at a time, so a
  // candidate never reaches the peer connection before its description.
  final backlog = <String>[];
  Future<void> Function(String)? current;
  var queue = Future<void>.value();
  void enqueue(String l) {
    final h = current!;
    queue = queue.then((_) => h(l)).catchError((e) => log('  signalling frame failed: $e'));
  }

  lines.listen((l) => current == null ? backlog.add(l) : enqueue(l));

  for (var n = 1; n <= rounds; n++) {
    log('=== CALL $n ($mode): set-up ===');
    final setup = Stopwatch()..start();
    final local = await openCamera();
    final pc = await makePc(mode, local,
        (c) => send({'cand': c.candidate, 'mid': c.sdpMid, 'idx': c.sdpMLineIndex}));
    final done = Completer<void>();
    current = frameHandler(pc, send, done);
    while (backlog.isNotEmpty) {
      enqueue(backlog.removeAt(0));
    }
    if (mode == 'offer') {
      final o = await pc.createOffer();
      await timed('setLocalDescription', () => pc.setLocalDescription(o));
      send({'sdp': o.sdp, 'type': o.type});
    }
    await waitConnected(pc, mode);
    log('CALL $n set-up total ${setup.elapsedMilliseconds} ms');
    phase = 'in call $n';
    await Future.delayed(Duration(seconds: holdSeconds * 2));
    log('  stats: ${await statsSummary(pc)}');
    if (mode == 'offer') {
      send({'bye': true});
    } else {
      await done.future;
    }
    current = null;
    log('=== CALL $n ($mode): hang-up ===');
    await timed('close', () => pc.close());
    for (final t in local.getTracks()) {
      await t.stop();
    }
    await local.dispose();
    await pc.dispose();
    phase = 'idle';
    await Future.delayed(const Duration(seconds: 2));
  }
  await sock.close();
}

Future<void> Function(String) frameHandler(RTCPeerConnection pc, void Function(Map<String, dynamic>) send,
        Completer<void> done) =>
    (String l) async {
      final m = jsonDecode(l) as Map<String, dynamic>;
      if (m['sdp'] != null) {
        await pc.setRemoteDescription(RTCSessionDescription(m['sdp'], m['type']));
        if (m['type'] == 'offer') {
          final ans = await pc.createAnswer();
          await timed('setLocalDescription', () => pc.setLocalDescription(ans));
          send({'sdp': ans.sdp, 'type': ans.type});
        }
      } else if (m['cand'] != null) {
        final c = RTCIceCandidate(m['cand'], m['mid'], m['idx']);
        final (addr, typ, proto) = describe(m['cand']);
        log('  remote candidate $typ $proto $addr');
        await pc.addCandidate(c);
      } else if (m['bye'] == true) {
        if (!done.isCompleted) done.complete();
      }
    };

// ---- app -----------------------------------------------------------------

void main() {
  WidgetsFlutterBinding.ensureInitialized();
  SchedulerBinding.instance.addTimingsCallback(onTimings);
  Timer.periodic(const Duration(milliseconds: 10), (_) => timerGaps.tick());
  runApp(const MaterialApp(home: Scaffold(body: Center(child: CircularProgressIndicator()))));
  Future.delayed(const Duration(seconds: 2), () async {
    log('mode $mode, rounds $rounds, flutter_webrtc 1.6.3');
    try {
      if (mode == 'loop') {
        await loopMode();
      } else {
        await pairMode();
      }
    } catch (e, st) {
      log('ERROR $e\n$st');
    }
    log('--- summary ---');
    log('candidate kinds: $candidateKinds');
    log('worst timer gap by phase (ms): ${timerGaps.worstByPhase}');
    log('worst frame gap by phase (ms): $worstFrameGapByPhase');
    log('timer gaps > 100 ms: ${timerGaps.big.length}');
    log('DONE');
    await Future.delayed(const Duration(milliseconds: 300));
    exit(0);
  });
}
