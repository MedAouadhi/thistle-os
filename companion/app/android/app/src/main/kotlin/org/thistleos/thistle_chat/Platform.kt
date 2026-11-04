package org.thistleos.thistle_chat

import android.Manifest
import android.app.Activity
import android.app.NotificationManager
import android.content.Context
import android.content.Intent
import android.content.pm.PackageManager
import android.net.Uri
import android.os.Build
import android.provider.Settings
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import java.util.WeakHashMap

/**
 * The Dart side's `org.thistleos.thistle_chat/platform` channel
 * (lib/src/android.dart): the Keystore vault, the listening service, the
 * runtime permissions, the device's name, a call's ring ([CallRing]) and
 * its window ([CallActivity]) ([Audio] has the audio's own channels).
 * Permissions are prompted for only while a window is there to show the
 * prompt.
 */
class Platform private constructor(private val context: Context, engine: FlutterEngine) :
    MethodChannel.MethodCallHandler {
    companion object {
        private val all = WeakHashMap<FlutterEngine, Platform>()
        private const val ASKED = "permissions"

        @Synchronized
        fun of(engine: FlutterEngine, context: Context): Platform =
            all.getOrPut(engine) { Platform(context.applicationContext, engine) }

        @Synchronized
        fun forget(engine: FlutterEngine) {
            all.remove(engine)?.let {
                it.audio.release()
                CallRing.stopAll(it.context)
            }
        }
    }

    var activity: Activity? = null

    /** The call window over the lock screen, while it is attached. */
    var callWindow: Activity? = null
    private val channel = MethodChannel(engine.dartExecutor.binaryMessenger, "org.thistleos.thistle_chat/platform")
    private val pending = mutableMapOf<Int, Pair<String, MethodChannel.Result>>()
    private var nextRequest = 4810

    /** The microphone and speakers, on channels of their own. */
    val audio = Audio(context, engine)

    init {
        channel.setMethodCallHandler(this)
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        try {
            when (call.method) {
                "vault.read" -> result.success(KeystoreVault.read(context, call.argument<String>("profile")!!))
                "vault.write" -> {
                    KeystoreVault.write(context, call.argument<String>("profile")!!, call.argument<String>("seed")!!)
                    result.success(null)
                }
                "service.set" -> {
                    if (call.argument<Boolean>("on") == true) ListenService.start(context) else ListenService.stop(context)
                    result.success(null)
                }
                "permission.status" -> result.success(status(call.argument<String>("name")!!))
                "permission.request" -> request(call.argument<String>("name")!!, result)
                "settings.open" -> {
                    val intent = Intent(
                        Settings.ACTION_APPLICATION_DETAILS_SETTINGS,
                        Uri.parse("package:${context.packageName}"),
                    )
                    (activity ?: context).startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                    result.success(null)
                }
                "device.name" -> result.success(deviceName())
                "ring.start" -> {
                    CallRing.start(
                        context,
                        call.argument<String>("caller")!!,
                        call.argument<String>("id")!!,
                        call.argument<Boolean>("notify") == true,
                    )
                    result.success(null)
                }
                "ring.stop" -> {
                    CallRing.stop(context, call.argument<String>("id")!!)
                    result.success(null)
                }
                "fillscreen.can" -> result.success(
                    Build.VERSION.SDK_INT < 34 ||
                        context.getSystemService(NotificationManager::class.java).canUseFullScreenIntent(),
                )
                "fillscreen.open" -> {
                    val intent = if (Build.VERSION.SDK_INT >= 34) {
                        Intent(Settings.ACTION_MANAGE_APP_USE_FULL_SCREEN_INTENT, Uri.parse("package:${context.packageName}"))
                    } else {
                        Intent(Settings.ACTION_APPLICATION_DETAILS_SETTINGS, Uri.parse("package:${context.packageName}"))
                    }
                    (activity ?: context).startActivity(intent.addFlags(Intent.FLAG_ACTIVITY_NEW_TASK))
                    result.success(null)
                }
                "callwindow.finish" -> {
                    callWindow?.finish()
                    result.success(null)
                }
                else -> result.notImplemented()
            }
        } catch (e: Exception) {
            result.error("platform", e.toString(), null)
        }
    }

    /** Tell Dart something (CallActivity, DeclineReceiver); on the main thread. */
    fun toDart(method: String, args: Any?) = channel.invokeMethod(method, args)

    private fun deviceName(): String {
        val named = if (Build.VERSION.SDK_INT >= 25) {
            Settings.Global.getString(context.contentResolver, Settings.Global.DEVICE_NAME)
        } else {
            null
        }
        return named?.takeIf { it.isNotBlank() } ?: Build.MODEL
    }

    /** The Android permission behind a name; null where none is prompted for. */
    private fun permission(name: String): String? = when (name) {
        "notifications" -> if (Build.VERSION.SDK_INT >= 33) Manifest.permission.POST_NOTIFICATIONS else null
        "microphone" -> Manifest.permission.RECORD_AUDIO
        else -> throw IllegalArgumentException("no permission $name")
    }

    private fun asked(permission: String) =
        context.getSharedPreferences(ASKED, Context.MODE_PRIVATE).getBoolean(permission, false)

    private fun status(name: String): String {
        val permission = permission(name)
        if (permission == null) {
            // Before Android 13 there is no prompt: only the app's settings switch notifications off.
            val on = context.getSystemService(NotificationManager::class.java).areNotificationsEnabled()
            return if (on) "granted" else "permanentlyDenied"
        }
        if (context.checkSelfPermission(permission) == PackageManager.PERMISSION_GRANTED) return "granted"
        val window = activity
        // Asked before, and Android would not show the prompt again: only Settings can allow it.
        if (asked(permission) && window != null && !window.shouldShowRequestPermissionRationale(permission)) {
            return "permanentlyDenied"
        }
        return "denied"
    }

    private fun request(name: String, result: MethodChannel.Result) {
        val permission = permission(name)
        val window = activity
        if (permission == null || window == null) {
            result.success(status(name)) // no prompt to show, or no window to show it in
            return
        }
        context.getSharedPreferences(ASKED, Context.MODE_PRIVATE).edit().putBoolean(permission, true).apply()
        val code = nextRequest++
        pending[code] = permission to result
        window.requestPermissions(arrayOf(permission), code)
    }

    fun onPermissionResult(code: Int, grants: IntArray) {
        val (permission, result) = pending.remove(code) ?: return
        val answer = when {
            grants.isEmpty() -> "denied" // the prompt was cancelled
            grants[0] == PackageManager.PERMISSION_GRANTED -> "granted"
            activity?.shouldShowRequestPermissionRationale(permission) == true -> "denied"
            else -> "permanentlyDenied"
        }
        result.success(answer)
    }
}
