package org.thistleos.thistle_chat

import android.annotation.SuppressLint
import android.app.Activity
import android.content.Context
import android.media.AudioAttributes
import android.media.AudioFormat
import android.media.AudioManager
import android.media.AudioRecord
import android.media.AudioTrack
import android.media.MediaRecorder
import android.media.audiofx.AcousticEchoCanceler
import android.media.audiofx.NoiseSuppressor
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.util.Log
import android.view.WindowManager
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.EventChannel
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.util.concurrent.LinkedBlockingQueue

/**
 * The Dart side's audio channels (lib/src/audio/android_audio.dart):
 * `org.thistleos.thistle_chat/audio` opens and closes the microphone and
 * speakers and takes the speakers' PCM; `.../microphone` carries the
 * microphone's frames. 16 kHz mono 16-bit, 20 ms (640 bytes) a frame.
 *
 * A call (`call.begin` .. `call.end`) puts the phone in communication mode,
 * records from the voice-communication source with the platform's
 * AcousticEchoCanceler where there is one, plays on the voice-call stream,
 * turns the screen off at the ear (proximity), and has [ListenService] in
 * the foreground as using the microphone, so the call keeps its audio with
 * the app in the background (Android 14).
 *
 * A WebRTC call (`rtc.begin` .. `rtc.end`) has libwebrtc record and play,
 * and flutter_webrtc's audio switch set the mode and the output; here it
 * only has the service in the foreground as using the microphone, and the
 * screen: dark at the ear (proximity) while no video shows, kept on while
 * one does (`rtc.video`), on whichever window is attached, the main one or
 * the call window.
 */
class Audio(private val context: Context, engine: FlutterEngine) :
    MethodChannel.MethodCallHandler, EventChannel.StreamHandler {
    companion object {
        private const val TAG = "ThistleChatAudio"
        private const val RATE = 16000
        private const val FRAME_BYTES = RATE / 50 * 2 // 20 ms
    }

    private val main = Handler(Looper.getMainLooper())
    private val control = MethodChannel(engine.dartExecutor.binaryMessenger, "org.thistleos.thistle_chat/audio")
    private val frames = EventChannel(engine.dartExecutor.binaryMessenger, "org.thistleos.thistle_chat/microphone")
    private var sink: EventChannel.EventSink? = null
    private var mic: Microphone? = null
    private val speakers = mutableMapOf<Int, Speaker>()
    private var nextSpeaker = 1
    private var inCall = false
    private var savedMode = AudioManager.MODE_NORMAL
    private var proximity: PowerManager.WakeLock? = null
    private var inRtc = false
    private var keepScreenOn = false

    /** The window attached to the engine, if one is: for keeping the screen on. */
    var window: Activity? = null
        set(value) {
            if (field !== value) field?.let { keepOn(it, false) }
            field = value
            value?.let { keepOn(it, keepScreenOn) }
        }
    private val audioManager = context.getSystemService(AudioManager::class.java)

    init {
        control.setMethodCallHandler(this)
        frames.setStreamHandler(this)
    }

    override fun onListen(arguments: Any?, events: EventChannel.EventSink) {
        sink = events
    }

    override fun onCancel(arguments: Any?) {
        sink = null
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        try {
            when (call.method) {
                "mic.open" -> result.success(openMicrophone(call.argument<Boolean>("call") == true))
                "mic.close" -> {
                    mic?.stop()
                    mic = null
                    result.success(null)
                }
                "speaker.open" -> {
                    val id = nextSpeaker++
                    speakers[id] = Speaker(call.argument<String>("use") ?: "voice")
                    result.success(id)
                }
                "speaker.write" -> {
                    speakers[call.argument<Int>("id")]?.write(call.argument<ByteArray>("pcm")!!)
                    result.success(null)
                }
                "speaker.close" -> {
                    val speaker = speakers.remove(call.argument<Int>("id"))
                    if (speaker == null) result.success(null) else speaker.close { main.post { result.success(null) } }
                }
                "call.begin" -> {
                    beginCall()
                    result.success(null)
                }
                "call.end" -> {
                    endCall()
                    result.success(null)
                }
                "rtc.begin" -> {
                    beginRtc()
                    result.success(null)
                }
                "rtc.video" -> {
                    rtcVideo(call.argument<Boolean>("shown") == true)
                    result.success(null)
                }
                "rtc.end" -> {
                    endRtc()
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        } catch (e: Exception) {
            Log.w(TAG, "${call.method}: $e")
            result.error("audio", e.message ?: e.toString(), null)
        }
    }

    /** Everything stops: the engine is going. */
    fun release() {
        mic?.stop()
        mic = null
        speakers.values.forEach { it.close {} }
        speakers.clear()
        endCall()
        endRtc()
    }

    /** Opens the microphone; what cancels its echo for a call, or null. */
    private fun openMicrophone(forCall: Boolean): String? {
        mic?.stop()
        val m = Microphone(forCall)
        mic = m
        return m.echo
    }

    private fun beginCall() {
        if (inCall) return
        inCall = true
        savedMode = audioManager.mode
        audioManager.mode = AudioManager.MODE_IN_COMMUNICATION
        darkAtTheEar(true)
        ListenService.call(context, true)
    }

    private fun endCall() {
        if (!inCall) return
        inCall = false
        audioManager.mode = savedMode
        darkAtTheEar(false)
        ListenService.call(context, false)
    }

    /** A WebRTC call: the service and the screen, no mode (libwebrtc's audio switch has it). */
    private fun beginRtc() {
        if (inRtc) return
        inRtc = true
        darkAtTheEar(true)
        ListenService.call(context, true)
    }

    /** Video on screen: the screen stays on and lit; none: dark at the ear again. */
    private fun rtcVideo(shown: Boolean) {
        if (!inRtc) return
        darkAtTheEar(!shown)
        keepScreenOn = shown
        window?.let { keepOn(it, shown) }
    }

    private fun endRtc() {
        if (!inRtc) return
        inRtc = false
        darkAtTheEar(false)
        keepScreenOn = false
        window?.let { keepOn(it, false) }
        ListenService.call(context, false)
    }

    /** The proximity lock: the screen off while something is near it. */
    private fun darkAtTheEar(on: Boolean) {
        if (!on) {
            proximity?.release()
            proximity = null
            return
        }
        if (proximity != null) return
        val power = context.getSystemService(PowerManager::class.java)
        if (power.isWakeLockLevelSupported(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK)) {
            proximity = power.newWakeLock(PowerManager.PROXIMITY_SCREEN_OFF_WAKE_LOCK, "thistle_chat:call").apply {
                setReferenceCounted(false)
                acquire(4 * 60 * 60 * 1000L) // a call's longest; released at its end
            }
        }
    }

    private fun keepOn(activity: Activity, on: Boolean) = activity.runOnUiThread {
        if (on) {
            activity.window.addFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        } else {
            activity.window.clearFlags(WindowManager.LayoutParams.FLAG_KEEP_SCREEN_ON)
        }
    }

    /** AudioRecord on a thread of its own, its frames to the event channel. */
    @SuppressLint("MissingPermission") // asked for from Dart (permissions.need) before any recording
    private inner class Microphone(forCall: Boolean) {
        private val record: AudioRecord
        private val echoCanceller: AcousticEchoCanceler?
        private val noiseSuppressor: NoiseSuppressor?
        val echo: String?

        @Volatile
        private var running = true
        private val thread: Thread

        init {
            val min = AudioRecord.getMinBufferSize(RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT)
            if (min <= 0) throw IllegalStateException("this phone cannot record 16 kHz mono")
            val source = if (forCall) MediaRecorder.AudioSource.VOICE_COMMUNICATION else MediaRecorder.AudioSource.MIC
            record = AudioRecord(source, RATE, AudioFormat.CHANNEL_IN_MONO, AudioFormat.ENCODING_PCM_16BIT, maxOf(min, FRAME_BYTES * 4))
            if (record.state != AudioRecord.STATE_INITIALIZED) {
                record.release()
                throw IllegalStateException("the microphone did not open (in use, or not allowed)")
            }
            echoCanceller = if (forCall && AcousticEchoCanceler.isAvailable()) {
                AcousticEchoCanceler.create(record.audioSessionId)?.apply { enabled = true }
            } else {
                null
            }
            noiseSuppressor = if (forCall && NoiseSuppressor.isAvailable()) {
                NoiseSuppressor.create(record.audioSessionId)?.apply { enabled = true }
            } else {
                null
            }
            echo = echoCanceller?.let { "on (Android's AcousticEchoCanceler)" }
            record.startRecording()
            thread = Thread({ run() }, "thistle-mic").apply { start() }
        }

        private fun run() {
            while (running) {
                val buf = ByteArray(FRAME_BYTES)
                var got = 0
                while (running && got < FRAME_BYTES) {
                    val n = record.read(buf, got, FRAME_BYTES - got)
                    if (n < 0) {
                        main.post { sink?.error("audio", "the microphone stopped ($n)", null) }
                        running = false
                        break
                    }
                    got += n
                }
                if (got == FRAME_BYTES) main.post { sink?.success(buf) }
            }
        }

        fun stop() {
            running = false
            try {
                record.stop()
            } catch (_: IllegalStateException) {
            }
            thread.join(500)
            echoCanceller?.release()
            noiseSuppressor?.release()
            record.release()
        }
    }

    /** AudioTrack, written from a thread of its own so the channel never blocks. */
    private class Speaker(use: String) {
        private val track: AudioTrack
        private val queue = LinkedBlockingQueue<ByteArray>()
        private val end = ByteArray(0)
        private var written = 0L
        private var whenDone: (() -> Unit)? = null

        init {
            val attributes = AudioAttributes.Builder()
                .setUsage(
                    when (use) {
                        "call" -> AudioAttributes.USAGE_VOICE_COMMUNICATION
                        "ring" -> AudioAttributes.USAGE_NOTIFICATION_RINGTONE
                        else -> AudioAttributes.USAGE_MEDIA
                    },
                )
                .setContentType(if (use == "ring") AudioAttributes.CONTENT_TYPE_SONIFICATION else AudioAttributes.CONTENT_TYPE_SPEECH)
                .build()
            val format = AudioFormat.Builder()
                .setSampleRate(RATE)
                .setChannelMask(AudioFormat.CHANNEL_OUT_MONO)
                .setEncoding(AudioFormat.ENCODING_PCM_16BIT)
                .build()
            val min = AudioTrack.getMinBufferSize(RATE, AudioFormat.CHANNEL_OUT_MONO, AudioFormat.ENCODING_PCM_16BIT)
            track = AudioTrack.Builder()
                .setAudioAttributes(attributes)
                .setAudioFormat(format)
                .setBufferSizeInBytes(maxOf(min, FRAME_BYTES * 4))
                .setTransferMode(AudioTrack.MODE_STREAM)
                .apply { if (Build.VERSION.SDK_INT >= 26) setPerformanceMode(AudioTrack.PERFORMANCE_MODE_LOW_LATENCY) }
                .build()
            if (track.state != AudioTrack.STATE_INITIALIZED) {
                track.release()
                throw IllegalStateException("the speaker did not open")
            }
            track.play()
            Thread({ run() }, "thistle-speaker").start()
        }

        fun write(pcm: ByteArray) {
            if (pcm.isNotEmpty()) queue.put(pcm)
        }

        /** Plays out what is queued, then releases; [done] runs on this speaker's thread. */
        fun close(done: () -> Unit) {
            whenDone = done
            queue.put(end)
        }

        private fun run() {
            while (true) {
                val pcm = queue.take()
                if (pcm === end) break
                var at = 0
                while (at < pcm.size) {
                    val n = track.write(pcm, at, pcm.size - at)
                    if (n <= 0) break
                    at += n
                }
                written += pcm.size / 2
            }
            // What is in the track's buffer plays out, at most half a second more.
            val deadline = System.currentTimeMillis() + 500
            while (track.playbackHeadPosition < written && System.currentTimeMillis() < deadline) Thread.sleep(10)
            track.stop()
            track.release()
            whenDone?.invoke()
        }
    }
}
