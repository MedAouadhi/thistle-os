import AVFoundation
import Cocoa
import FlutterMacOS
import Security

class MainFlutterWindow: NSWindow {
  // The app's own channels on macOS (lib/src/identity.dart's KeychainVault,
  // lib/src/audio/android_audio.dart's channel audio): kept for the window's
  // life.
  private var keychain: KeychainChannel?
  private var audio: AudioChannel?

  override func awakeFromNib() {
    let flutterViewController = FlutterViewController()
    let windowFrame = self.frame
    self.contentViewController = flutterViewController
    self.setFrame(windowFrame, display: true)
    self.minSize = NSSize(width: 360, height: 480)
    self.title = "Thistle Chat"

    RegisterGeneratedPlugins(registry: flutterViewController)
    let messenger = flutterViewController.engine.binaryMessenger
    keychain = KeychainChannel(messenger: messenger)
    audio = AudioChannel(messenger: messenger)

    super.awakeFromNib()
  }
}

// MARK: - The identity's seed in the login keychain

/// The identity key's seed (64 hex digits) as a generic password, service
/// `org.thistleos.thistle_chat`, account the profile. Never in a file.
final class KeychainChannel {
  private static let service = "org.thistleos.thistle_chat"
  private let channel: FlutterMethodChannel

  init(messenger: FlutterBinaryMessenger) {
    channel = FlutterMethodChannel(name: "org.thistleos.thistle_chat/keychain", binaryMessenger: messenger)
    channel.setMethodCallHandler { call, result in
      let args = call.arguments as? [String: Any] ?? [:]
      let profile = args["profile"] as? String ?? "default"
      switch call.method {
      case "read": result(KeychainChannel.read(profile))
      case "write": result(KeychainChannel.write(profile, args["seed"] as? String ?? ""))
      case "delete": result(KeychainChannel.delete(profile))
      default: result(FlutterMethodNotImplemented)
      }
    }
  }

  private static func query(_ profile: String) -> [String: Any] {
    [
      kSecClass as String: kSecClassGenericPassword,
      kSecAttrService as String: service,
      kSecAttrAccount as String: profile,
    ]
  }

  private static func read(_ profile: String) -> Any? {
    var q = query(profile)
    q[kSecReturnData as String] = true
    q[kSecMatchLimit as String] = kSecMatchLimitOne
    var out: CFTypeRef?
    let status = SecItemCopyMatching(q as CFDictionary, &out)
    if status == errSecItemNotFound { return nil }
    guard status == errSecSuccess, let data = out as? Data, let text = String(data: data, encoding: .utf8) else {
      return failure(status)
    }
    return text
  }

  private static func write(_ profile: String, _ seed: String) -> Any? {
    var q = query(profile)
    q[kSecValueData as String] = Data(seed.utf8)
    q[kSecAttrLabel as String] = "Thistle Chat identity (\(profile))"
    let status = SecItemAdd(q as CFDictionary, nil)
    return status == errSecSuccess ? nil : failure(status)
  }

  private static func delete(_ profile: String) -> Any? {
    let status = SecItemDelete(query(profile) as CFDictionary)
    return status == errSecSuccess || status == errSecItemNotFound ? nil : failure(status)
  }

  private static func failure(_ status: OSStatus) -> FlutterError {
    let why = SecCopyErrorMessageString(status, nil) as String? ?? "OSStatus \(status)"
    return FlutterError(code: "keychain", message: why, details: nil)
  }
}

// MARK: - The microphone and the speaker

/// The same channels as Android's Audio.kt: 16 kHz mono 16-bit, the
/// microphone's 20 ms frames on the event channel, the speaker's PCM written
/// over the method channel. A call's microphone and speaker share one
/// AVAudioEngine with Apple's voice processing on, which cancels the
/// speaker's echo from the microphone.
final class AudioChannel: NSObject, FlutterStreamHandler {
  private static let frameSamples = 320  // 20 ms at 16 kHz
  private let control: FlutterMethodChannel
  private let microphone: FlutterEventChannel
  private var sink: FlutterEventSink?

  private let pcm16 = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: 16000, channels: 1, interleaved: true)!
  private let float16 = AVAudioFormat(commonFormat: .pcmFormatFloat32, sampleRate: 16000, channels: 1, interleaved: false)!

  /// The microphone's engine: the call's (shared with its speaker) or its own.
  private var micEngine: AVAudioEngine?
  private var micIsCall = false
  /// Bumped at each open and close, so frames from a closed one are dropped.
  private var micGeneration = 0
  private var pending: [Int16] = []

  /// A call's engine and its speaker, made by mic.open with `call`.
  private var callEngine: AVAudioEngine?
  private var callPlayer: AVAudioPlayerNode?
  private var callSpeakerId: Int?

  private final class Speaker {
    init(engine: AVAudioEngine, node: AVAudioPlayerNode, ownsEngine: Bool) {
      self.engine = engine
      self.node = node
      self.ownsEngine = ownsEngine
    }
    let engine: AVAudioEngine
    let node: AVAudioPlayerNode
    let ownsEngine: Bool
  }
  private var speakers: [Int: Speaker] = [:]
  private var nextSpeaker = 1

  init(messenger: FlutterBinaryMessenger) {
    control = FlutterMethodChannel(name: "org.thistleos.thistle_chat/audio", binaryMessenger: messenger)
    microphone = FlutterEventChannel(name: "org.thistleos.thistle_chat/microphone", binaryMessenger: messenger)
    super.init()
    microphone.setStreamHandler(self)
    control.setMethodCallHandler { [weak self] call, result in
      guard let self else { return result(nil) }
      let args = call.arguments as? [String: Any] ?? [:]
      switch call.method {
      case "mic.open": self.openMicrophone(call: args["call"] as? Bool ?? false, result: result)
      case "mic.close":
        self.closeMicrophone()
        result(nil)
      case "speaker.open": result(self.openSpeaker(use: args["use"] as? String ?? "voice"))
      case "speaker.write":
        if let id = args["id"] as? Int, let data = args["pcm"] as? FlutterStandardTypedData {
          self.write(id, data.data)
        }
        result(nil)
      case "speaker.close": self.closeSpeaker(args["id"] as? Int ?? -1, result: result)
      case "call.begin": result(nil)
      case "call.end":
        self.endCall()
        result(nil)
      default: result(FlutterMethodNotImplemented)
      }
    }
  }

  // FlutterStreamHandler: the microphone's frames go to whoever listens.
  func onListen(withArguments arguments: Any?, eventSink events: @escaping FlutterEventSink) -> FlutterError? {
    sink = events
    return nil
  }

  func onCancel(withArguments arguments: Any?) -> FlutterError? {
    sink = nil
    return nil
  }

  // MARK: microphone

  private func openMicrophone(call: Bool, result: @escaping FlutterResult) {
    withMicrophoneAccess { [weak self] allowed in
      guard let self else { return result(nil) }
      guard allowed else {
        return result(
          FlutterError(
            code: "audio",
            message: "the microphone is not allowed (System Settings, Privacy & Security, Microphone)",
            details: nil))
      }
      do {
        result(try self.startMicrophone(call: call))
      } catch {
        if call { self.endCall() }
        result(FlutterError(code: "audio", message: "the microphone could not be opened: \(error)", details: nil))
      }
    }
  }

  private func withMicrophoneAccess(_ go: @escaping (Bool) -> Void) {
    switch AVCaptureDevice.authorizationStatus(for: .audio) {
    case .authorized: go(true)
    case .notDetermined:
      AVCaptureDevice.requestAccess(for: .audio) { ok in DispatchQueue.main.async { go(ok) } }
    default: go(false)
    }
  }

  /// Start the microphone; for a call, what cancels its echo.
  private func startMicrophone(call: Bool) throws -> String? {
    closeMicrophone()
    let engine = AVAudioEngine()
    var echo: String?
    if call {
      endCall()
      do {
        try engine.inputNode.setVoiceProcessingEnabled(true)
        echo = "on (macOS voice processing)"
      } catch {
        echo = "none (macOS voice processing would not start: \(error.localizedDescription))"
      }
      // The call's speaker, on the same engine, so voice processing hears
      // what it plays.
      let player = AVAudioPlayerNode()
      engine.attach(player)
      engine.connect(player, to: engine.mainMixerNode, format: float16)
      callEngine = engine
      callPlayer = player
    }
    let input = engine.inputNode
    let format = input.outputFormat(forBus: 0)
    guard format.sampleRate > 0, format.channelCount > 0 else {
      throw NSError(domain: "audio", code: 1, userInfo: [NSLocalizedDescriptionKey: "no microphone"])
    }
    guard let converter = AVAudioConverter(from: format, to: pcm16) else {
      throw NSError(domain: "audio", code: 2, userInfo: [NSLocalizedDescriptionKey: "no converter from \(format)"])
    }
    micGeneration += 1
    let generation = micGeneration
    let ratio = 16000 / format.sampleRate
    input.installTap(onBus: 0, bufferSize: 1024, format: format) { [weak self] buffer, _ in
      let capacity = AVAudioFrameCount(Double(buffer.frameLength) * ratio) + 64
      guard let self, let out = AVAudioPCMBuffer(pcmFormat: self.pcm16, frameCapacity: capacity) else { return }
      var given = false
      var error: NSError?
      _ = converter.convert(to: out, error: &error) { _, status in
        if given {
          status.pointee = .noDataNow
          return nil
        }
        given = true
        status.pointee = .haveData
        return buffer
      }
      guard error == nil, let samples = out.int16ChannelData, out.frameLength > 0 else { return }
      let chunk = Array(UnsafeBufferPointer(start: samples[0], count: Int(out.frameLength)))
      DispatchQueue.main.async { self.push(chunk, generation: generation) }
    }
    engine.prepare()
    do {
      try engine.start()
    } catch {
      input.removeTap(onBus: 0)
      throw error
    }
    if call { callPlayer?.play() }
    micEngine = engine
    micIsCall = call
    return echo
  }

  private func push(_ samples: [Int16], generation: Int) {
    guard generation == micGeneration else { return }
    pending.append(contentsOf: samples)
    let n = AudioChannel.frameSamples
    while pending.count >= n {
      let frame = pending.prefix(n)
      pending.removeFirst(n)
      let data = frame.withUnsafeBufferPointer { Data(buffer: $0) }
      sink?(FlutterStandardTypedData(bytes: data))
    }
  }

  private func closeMicrophone() {
    micGeneration += 1
    pending.removeAll()
    guard let engine = micEngine else { return }
    micEngine = nil
    engine.inputNode.removeTap(onBus: 0)
    // A call's engine plays its speaker too, until call.end.
    if !micIsCall { engine.stop() }
  }

  // MARK: speaker

  private func openSpeaker(use: String) -> Any? {
    let id = nextSpeaker
    nextSpeaker += 1
    if use == "call", let engine = callEngine, let player = callPlayer, callSpeakerId == nil {
      speakers[id] = Speaker(engine: engine, node: player, ownsEngine: false)
      callSpeakerId = id
      return id
    }
    let engine = AVAudioEngine()
    let player = AVAudioPlayerNode()
    engine.attach(player)
    engine.connect(player, to: engine.mainMixerNode, format: float16)
    engine.prepare()
    do {
      try engine.start()
    } catch {
      return FlutterError(code: "audio", message: "the speaker could not be opened: \(error)", details: nil)
    }
    player.play()
    speakers[id] = Speaker(engine: engine, node: player, ownsEngine: true)
    return id
  }

  private func write(_ id: Int, _ data: Data) {
    guard let speaker = speakers[id] else { return }
    let n = data.count / 2
    guard n > 0, let buffer = AVAudioPCMBuffer(pcmFormat: float16, frameCapacity: AVAudioFrameCount(n)) else { return }
    buffer.frameLength = AVAudioFrameCount(n)
    let out = buffer.floatChannelData![0]
    data.withUnsafeBytes { raw in
      for i in 0..<n { out[i] = Float(raw.loadUnaligned(fromByteOffset: i * 2, as: Int16.self)) / 32768 }
    }
    speaker.node.scheduleBuffer(buffer, completionHandler: nil)
  }

  /// Play out what was written, then let the speaker go.
  private func closeSpeaker(_ id: Int, result: @escaping FlutterResult) {
    guard let speaker = speakers.removeValue(forKey: id) else { return result(nil) }
    if id == callSpeakerId { callSpeakerId = nil }
    guard speaker.engine.isRunning, speaker.node.isPlaying else {
      if speaker.ownsEngine { speaker.engine.stop() }
      return result(nil)
    }
    // An empty buffer's completion marks the end of what was scheduled.
    let marker = AVAudioPCMBuffer(pcmFormat: float16, frameCapacity: 1)!
    marker.frameLength = 1
    marker.floatChannelData![0][0] = 0
    speaker.node.scheduleBuffer(marker, completionCallbackType: .dataPlayedBack) { _ in
      DispatchQueue.main.async {
        speaker.node.stop()
        if speaker.ownsEngine { speaker.engine.stop() }
        result(nil)
      }
    }
  }

  // MARK: call

  private func endCall() {
    if micIsCall { closeMicrophone() }
    micIsCall = false
    if let id = callSpeakerId { speakers.removeValue(forKey: id) }
    callSpeakerId = nil
    callPlayer?.stop()
    callPlayer = nil
    callEngine?.stop()
    callEngine = nil
  }
}
