package org.thistleos.thistle_chat

import android.content.Context
import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.engine.FlutterEngine

/**
 * The window onto the one Flutter engine ([Engines]). The engine, and the
 * chat peer listening in it, outlive this activity while [ListenService]
 * runs: swiping the app away closes the window, not the listener.
 */
class MainActivity : FlutterActivity() {
    /** The call window ([CallActivity]) took the engine from this window. */
    private var evicted = false

    override fun provideFlutterEngine(context: Context): FlutterEngine = Engines.get(context)

    override fun detachFromFlutterEngine() {
        evicted = true
        super.detachFromFlutterEngine()
    }

    /** Back in sight after a call window: attached again, by being made anew. */
    override fun onStart() {
        super.onStart()
        if (evicted) {
            evicted = false
            recreate()
        }
    }

    /** Destroy the engine with the window only when nothing keeps it listening. */
    override fun shouldDestroyEngineWithHost(): Boolean {
        val drop = isFinishing && !ListenService.running
        if (drop) Engines.forget()
        return drop
    }

    override fun configureFlutterEngine(flutterEngine: FlutterEngine) {
        super.configureFlutterEngine(flutterEngine)
        Platform.of(flutterEngine, applicationContext).activity = this
    }

    override fun cleanUpFlutterEngine(flutterEngine: FlutterEngine) {
        Platform.of(flutterEngine, applicationContext).let { if (it.activity === this) it.activity = null }
        super.cleanUpFlutterEngine(flutterEngine)
    }

    override fun onRequestPermissionsResult(requestCode: Int, permissions: Array<out String>, grantResults: IntArray) {
        super.onRequestPermissionsResult(requestCode, permissions, grantResults)
        Engines.current()?.let { Platform.of(it, applicationContext).onPermissionResult(requestCode, grantResults) }
    }
}
