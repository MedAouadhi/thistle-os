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

        fun start(context: Context) {
            val intent = Intent(context, ListenService::class.java)
            if (Build.VERSION.SDK_INT >= 26) context.startForegroundService(intent) else context.startService(intent)
        }

        fun stop(context: Context) {
            context.stopService(Intent(context, ListenService::class.java))
        }
    }

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
        val notification = builder
            .setSmallIcon(R.drawable.ic_stat_chat)
            .setContentTitle("Thistle Chat is listening")
            .setContentText("Messages and calls reach this phone over your tailnet. Switch it off in Settings.")
            .setContentIntent(open)
            .setOngoing(true)
            .setShowWhen(false)
            .build()
        if (Build.VERSION.SDK_INT >= 34) {
            startForeground(NOTIFICATION, notification, ServiceInfo.FOREGROUND_SERVICE_TYPE_REMOTE_MESSAGING)
        } else {
            startForeground(NOTIFICATION, notification)
        }
    }

    override fun onStartCommand(intent: Intent?, flags: Int, startId: Int): Int {
        Engines.get(this) // the engine already runs, unless Android restarted us
        return START_STICKY
    }

    override fun onDestroy() {
        running = false
        super.onDestroy()
    }

    override fun onBind(intent: Intent?): IBinder? = null
}
