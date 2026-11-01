package org.thistleos.thistle_chat

import android.content.Context
import io.flutter.embedding.engine.FlutterEngine
import io.flutter.embedding.engine.FlutterEngineCache
import io.flutter.embedding.engine.dart.DartExecutor

/**
 * The app's one Flutter engine, made by whichever comes first: the window
 * ([MainActivity]) or the listening service after Android restarted it
 * ([ListenService]). Its `main()` starts the chat peer either way.
 */
object Engines {
    private const val ID = "thistle_chat"

    fun current(): FlutterEngine? = FlutterEngineCache.getInstance().get(ID)

    @Synchronized
    fun get(context: Context): FlutterEngine {
        current()?.let { return it }
        val app = context.applicationContext
        val engine = FlutterEngine(app)
        Platform.of(engine, app) // the channel is there before main() calls it
        engine.dartExecutor.executeDartEntrypoint(DartExecutor.DartEntrypoint.createDefault())
        FlutterEngineCache.getInstance().put(ID, engine)
        return engine
    }

    /** The engine is about to be destroyed: the next window makes another. */
    @Synchronized
    fun forget() {
        current()?.let { Platform.forget(it) }
        FlutterEngineCache.getInstance().remove(ID)
    }
}
