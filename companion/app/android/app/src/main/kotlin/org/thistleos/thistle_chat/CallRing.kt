package org.thistleos.thistle_chat

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Person
import android.content.BroadcastReceiver
import android.content.Context
import android.content.Intent
import android.media.AudioAttributes
import android.media.AudioManager
import android.media.Ringtone
import android.media.RingtoneManager
import android.os.Build
import android.os.Handler
import android.os.Looper
import android.os.VibrationAttributes
import android.os.VibrationEffect
import android.os.Vibrator
import android.os.VibratorManager
import android.provider.Settings
import android.util.Log

/**
 * A call coming in rings like a phone call (tailnet-video spec, A call to
 * a locked phone): the phone's default ringtone, looping on the ringtone
 * stream, with a repeating vibration, both as the ringer mode and Do Not
 * Disturb say; and, with the app out of sight, Android's call-style
 * notification, silent itself, whose full-screen intent opens
 * [CallActivity] over the lock screen. Started and stopped from Dart
 * (notifier.dart, talk.dart), which stops it at the answer, the decline,
 * the caller's end and the 45 s timeout.
 */
object CallRing {
    /**
     * Not `calls`: flutter_local_notifications made that channel with a
     * sound, and Android keeps a channel's sound once made, so the
     * notification would ring over the ringtone.
     */
    private const val CHANNEL = "incoming_calls"
    private const val OLD_CHANNEL = "calls"
    private const val NOTIFICATION = 4811
    const val EXTRA_ID = "call"
    const val EXTRA_ANSWER = "answer"

    private var ringing: String? = null
    private var ringtone: Ringtone? = null
    private var vibrator: Vibrator? = null
    private val main = Handler(Looper.getMainLooper())

    /** Before Android 9 a ringtone does not loop: it is played again. */
    private val again = object : Runnable {
        override fun run() {
            val r = ringtone ?: return
            if (!r.isPlaying) r.play()
            main.postDelayed(this, 1000)
        }
    }

    @Synchronized
    fun start(context: Context, caller: String, id: String, notify: Boolean, video: Boolean = false) {
        if (ringing == id) return
        ringing?.let { stop(context, it) }
        ringing = id
        val (sound, vibrate) = alerts(context)
        if (sound) playRingtone(context)
        if (vibrate) vibrate(context)
        if (notify) notify(context, caller, id, video)
        Log.i("ThistleChat", "ringing for a call: ringtone $sound, vibration $vibrate, notification $notify")
    }

    @Synchronized
    fun stop(context: Context, id: String) {
        if (ringing != id) return
        ringing = null
        main.removeCallbacks(again)
        ringtone?.stop()
        ringtone = null
        vibrator?.cancel()
        vibrator = null
        context.getSystemService(NotificationManager::class.java).cancel(NOTIFICATION)
    }

    /** The engine went: nothing is left to stop the ring. */
    @Synchronized
    fun stopAll(context: Context) {
        ringing?.let { stop(context, it) }
    }

    /**
     * Whether to sound and to vibrate: the ringer mode, and Do Not
     * Disturb's call rules. A tailnet caller is no contact of the phone's,
     * so only "calls from anyone" lets one through.
     */
    private fun alerts(context: Context): Pair<Boolean, Boolean> {
        val notifications = context.getSystemService(NotificationManager::class.java)
        val allowed = when (notifications.currentInterruptionFilter) {
            NotificationManager.INTERRUPTION_FILTER_ALL, NotificationManager.INTERRUPTION_FILTER_UNKNOWN -> true
            NotificationManager.INTERRUPTION_FILTER_PRIORITY -> try {
                val policy = notifications.notificationPolicy
                policy.priorityCategories and NotificationManager.Policy.PRIORITY_CATEGORY_CALLS != 0 &&
                    policy.priorityCallSenders == NotificationManager.Policy.PRIORITY_SENDERS_ANY
            } catch (e: SecurityException) {
                false
            }
            else -> false // alarms only, or nothing
        }
        if (!allowed) return false to false
        return when (context.getSystemService(AudioManager::class.java).ringerMode) {
            AudioManager.RINGER_MODE_NORMAL ->
                true to (Settings.System.getInt(context.contentResolver, Settings.System.VIBRATE_WHEN_RINGING, 0) != 0)
            AudioManager.RINGER_MODE_VIBRATE -> false to true
            else -> false to false
        }
    }

    private fun playRingtone(context: Context) {
        val uri = RingtoneManager.getActualDefaultRingtoneUri(context, RingtoneManager.TYPE_RINGTONE)
            ?: Settings.System.DEFAULT_RINGTONE_URI
        val r = RingtoneManager.getRingtone(context, uri) ?: return
        r.audioAttributes = AudioAttributes.Builder()
            .setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE)
            .setContentType(AudioAttributes.CONTENT_TYPE_SONIFICATION)
            .build()
        ringtone = r
        if (Build.VERSION.SDK_INT >= 28) {
            r.isLooping = true
            r.play()
        } else {
            again.run()
        }
    }

    /** A second on, a second off, until stopped. */
    private fun vibrate(context: Context) {
        val v = if (Build.VERSION.SDK_INT >= 31) {
            context.getSystemService(VibratorManager::class.java).defaultVibrator
        } else {
            @Suppress("DEPRECATION")
            context.getSystemService(Vibrator::class.java)
        }
        if (!v.hasVibrator()) return
        vibrator = v
        val pattern = longArrayOf(0, 1000, 1000)
        when {
            Build.VERSION.SDK_INT >= 33 -> v.vibrate(
                VibrationEffect.createWaveform(pattern, 0),
                VibrationAttributes.createForUsage(VibrationAttributes.USAGE_RINGTONE),
            )
            Build.VERSION.SDK_INT >= 26 -> @Suppress("DEPRECATION") v.vibrate(
                VibrationEffect.createWaveform(pattern, 0),
                AudioAttributes.Builder().setUsage(AudioAttributes.USAGE_NOTIFICATION_RINGTONE).build(),
            )
            else -> @Suppress("DEPRECATION") v.vibrate(pattern, 0)
        }
    }

    /** A video call says so; its window offers Answer with video (call_page.dart). */
    private fun notify(context: Context, caller: String, id: String, video: Boolean) {
        val notifications = context.getSystemService(NotificationManager::class.java)
        if (Build.VERSION.SDK_INT >= 26) {
            notifications.deleteNotificationChannel(OLD_CHANNEL)
            val channel = NotificationChannel(CHANNEL, "Calls", NotificationManager.IMPORTANCE_HIGH)
            channel.description = "Calls coming in. The ring is the phone's ringtone, played by the app."
            channel.setSound(null, null)
            channel.enableVibration(false)
            channel.lockscreenVisibility = Notification.VISIBILITY_PUBLIC
            notifications.createNotificationChannel(channel)
        }
        val flags = PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT
        fun window(answer: Boolean) = Intent(context, CallActivity::class.java)
            .putExtra(EXTRA_ID, id)
            .putExtra(EXTRA_ANSWER, answer)
            .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
        val show = PendingIntent.getActivity(context, 1, window(false), flags)
        val answer = PendingIntent.getActivity(context, 2, window(true), flags)
        val decline = PendingIntent.getBroadcast(
            context,
            3,
            Intent(context, DeclineReceiver::class.java).putExtra(EXTRA_ID, id),
            flags,
        )
        @Suppress("DEPRECATION")
        val builder = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(context, CHANNEL) else Notification.Builder(context)
        builder
            .setSmallIcon(R.drawable.ic_stat_chat)
            .setContentTitle(caller)
            .setContentText(if (video) "Tailnet video call" else "Tailnet call")
            .setCategory(Notification.CATEGORY_CALL)
            .setVisibility(Notification.VISIBILITY_PUBLIC)
            .setOngoing(true)
            .setContentIntent(show)
            // Opens the call window on a locked phone; a heads-up otherwise,
            // or always where Android withholds the permission.
            .setFullScreenIntent(show, true)
        if (Build.VERSION.SDK_INT >= 31) {
            val person = Person.Builder().setName(caller).setImportant(true).build()
            builder.setStyle(Notification.CallStyle.forIncomingCall(person, decline, answer).setIsVideo(video))
        } else {
            @Suppress("DEPRECATION")
            builder.setPriority(Notification.PRIORITY_MAX)
                .setSound(null)
                .addAction(Notification.Action.Builder(null, "Decline", decline).build())
                .addAction(Notification.Action.Builder(null, "Answer", answer).build())
        }
        notifications.notify(NOTIFICATION, builder.build())
    }
}

/**
 * The call notification's Decline: stops the ring at once and tells the
 * engine whose chat peer has the call, with no window.
 */
class DeclineReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        val id = intent.getStringExtra(CallRing.EXTRA_ID) ?: return
        CallRing.stop(context, id)
        Engines.current()?.let { Platform.of(it, context).toDart("call.decline", mapOf("id" to id)) }
    }
}
