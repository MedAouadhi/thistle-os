package org.thistleos.thistle_chat

import android.content.Context
import android.content.Intent
import android.os.Build
import android.os.Bundle
import android.view.WindowManager
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine

/**
 * The call window (tailnet-video spec, A call to a locked phone): opened
 * by the call notification ([CallRing]), shown over the lock screen with
 * the screen turned on, on the one Flutter engine ([Engines]). While it is
 * attached the app draws the call screen and nothing else (call_page.dart,
 * CallWindowed); Dart finishes it when the call screen closes, so a locked
 * phone is back at its lock screen. Answering needs no unlock. Its own
 * task, kept out of the recent apps: finishing it never brings the chats
 * forward. [MainActivity] stays off the lock screen.
 */
class CallActivity : FlutterActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        if (Build.VERSION.SDK_INT >= 27) {
            setShowWhenLocked(true)
            setTurnScreenOn(true)
        } else {
            @Suppress("DEPRECATION")
            window.addFlags(WindowManager.LayoutParams.FLAG_SHOW_WHEN_LOCKED or WindowManager.LayoutParams.FLAG_TURN_SCREEN_ON)
        }
        super.onCreate(savedInstanceState)
        answerIfAsked(intent)
    }

    override fun onNewIntent(intent: Intent) {
        super.onNewIntent(intent)
        setIntent(intent)
        answerIfAsked(intent)
    }

    /** The notification's Answer opens the window and answers. */
    private fun answerIfAsked(intent: Intent) {
        val id = intent.getStringExtra(CallRing.EXTRA_ID) ?: return
        if (!intent.getBooleanExtra(CallRing.EXTRA_ANSWER, false)) return
        intent.removeExtra(CallRing.EXTRA_ANSWER) // not again on a recreate
        Engines.current()?.let { Platform.of(it, applicationContext).toDart("call.answer", mapOf("id" to id)) }
    }

    override fun provideFlutterEngine(context: Context): FlutterEngine = Engines.get(context)

    /** The engine is the listener's and the main window's too. */
    override fun shouldDestroyEngineWithHost(): Boolean = false

    /** Attached, before the first frame: Dart draws the call alone from now. */
    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        Platform.of(flutterEngine, applicationContext).let {
            it.activity = this
            it.callWindow = this
            it.toDart("call.window", mapOf("shown" to true))
        }
    }

    override fun cleanUpFlutterEngine(flutterEngine: FlutterEngine) {
        Platform.of(flutterEngine, applicationContext).let {
            if (it.activity === this) it.activity = null
            if (it.callWindow === this) {
                it.callWindow = null
                it.toDart("call.window", mapOf("shown" to false))
            }
        }
        super.cleanUpFlutterEngine(flutterEngine)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        Engines.current()?.let { Platform.of(it, applicationContext).onPermissionResult(requestCode, grantResults) }
    }
}
