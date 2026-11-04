package org.thistleos.thistle_chat

import android.app.Notification
import android.app.NotificationChannel
import android.app.NotificationManager
import android.app.PendingIntent
import android.app.Service
import android.content.Context
import android.content.Intent
import android.content.pm.ServiceInfo
import android.os.Build
import android.os.IBinder
import android.util.Log

/**
 * Keeps the app reachable in the background (spec, The companion app:
 * Listening): a foreground service with a persistent notification, so the
 * process and the Flutter engine whose chat peer listens on the tailnet
 * address stay alive after the app is swiped away. Started and stopped
 * from Dart (Settings, Listen in the background; on by default). If
 * Android kills the process, it restarts the service, which starts the
 * engine again with no window.
 */
class ListenService : Service() {
    companion object {
        @Volatile
        var running = false
            private set

        private const val CHANNEL = "listening"
        private const val NOTIFICATION = 4810

        /** Listening in the background, as Settings has it. */
        @Volatile
        private var listening = false

        /** A call is up: the service is in the foreground as using the microphone too. */
        @Volatile
        private var inCall = false

        fun start(context: Context) {
            listening = true
            launch(context)
        }

        fun stop(context: Context) {
            listening = false
            if (!inCall) context.stopService(Intent(context, ListenService::class.java))
        }

        /**
         * A call began or ended (Audio.kt). Begun from the foreground (the
         * call screen, or a notification's Answer that opened it), as
         * Android 14 starts a microphone service from nowhere else; the
         * call then keeps its microphone with the app in the background.
         */
        fun call(context: Context, on: Boolean) {
            inCall = on
            if (on || listening) launch(context) else context.stopService(Intent(context, ListenService::class.java))
        }

        private fun launch(context: Context) {
            val intent = Intent(context, ListenService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent) else context.startService(intent)
        }
    }

    private lateinit var notification: Notification

    override fun onCreate() {
        super.onCreate()
        running = true
        if (Build.VERSION.SDK_INT >= 26) {
            val channel = NotificationChannel(CHANNEL, "Listening", NotificationManager.IMPORTANCE_LOW)
            channel.description = "Shown while Thistle Chat listens for messages and calls in the background"
            channel.setShowBadge(false)
            getSystemService(NotificationManager::class.java).createNotificationChannel(channel)
        }
        val open = PendingIntent.getActivity(
            this,
            0,
            Intent(this, MainActivity::class.java).addFlags(Intent.FLAG_ACTIVITY_SINGLE_TOP),
            PendingIntent.FLAG_IMMUTABLE or PendingIntent.FLAG_UPDATE_CURRENT,
        )
        @Suppress("DEPRECATION")
        val builder = if (Build.VERSION.SDK_INT >= 26) Notification.Builder(this, CHANNEL) else Notification.Builder(this)
        notification = builder
            .setSmallIcon(R.drawable.ic_stat_chat)
            .setContentTitle("Thistle Chat is listening")
            .setContentText("Messages and calls reach this phone over your tailnet. Switch it off in Settings.")
            .setContentIntent(open)
            .setOngoing(true)
            .setShowWhen(false)
            .build()
        foreground()
    }

    /** In the foreground as listening, and as using the microphone while a call is up. */
    private fun foreground() {
        if (Build.VERSION.SDK_INT < 29) {
            startForeground(NOTIFICATION, notification)
            return
        }
        val listen = if (Build.VERSION.SDK_INT >= 34) ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING else 0
        val microphone = if (inCall && Build.VERSION.SDK_INT >= 30) ServiceInfo.FOREGROUND_SERVICE_TYPE_MICROPHONE else 0
        try {
            startForeground(NOTIFICATION, notification, listen or microphone)
        } catch (e: SecurityException) {
            // Not allowed the microphone type (no permission, or not begun
            // from the foreground): the call keeps its audio only while
            // the app is on screen.
            Log.w("ThistleChat", "no microphone service type: $e")
            startForeground(NOTIFICATION, notification, listen)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        // Restarted by Android after it stopped the process: it was listening.
        if (intent == null) listening = true
        foreground()
        Engines.get(this) // the engine already runs, unless Android restarted us
        return START_STICKY
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
