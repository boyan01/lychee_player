package tech.soit.lychee.lychee_player

import android.content.Context
import android.hardware.display.DisplayManager
import android.view.Display
import android.app.Activity
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.embedding.engine.plugins.activity.ActivityAware
import io.flutter.embedding.engine.plugins.activity.ActivityPluginBinding
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry
import tech.soit.lychee.LycheePlayer

class LycheePlayerPlugin : FlutterPlugin, ActivityAware, MethodChannel.MethodCallHandler {
    private lateinit var channel: MethodChannel
    private lateinit var textureRegistry: TextureRegistry
    private lateinit var context: Context
    private val textures = HashMap<Long, LycheeTexture>()
    // Whose window display mode matching asks for modes, and where
    // SurfaceViews go.
    private var activity: Activity? = null

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        System.loadLibrary("lychee")
        if (BuildConfig.LYCHEE_WITH_FFMPEG) {
            // The extension must be registered before Dart creates any player.
            // Reflection keeps the base-only build independent of its classes.
            Class.forName("tech.soit.lychee.LycheeFFmpeg")
                .getMethod("load").invoke(null)
        }
        context = binding.applicationContext
        // Dart's players open content:// URIs through it.
        LycheePlayer.setContext(context)
        textureRegistry = binding.textureRegistry
        channel = MethodChannel(binding.binaryMessenger, "lychee_player")
        channel.setMethodCallHandler(this)
    }

    // Also when an activity's engine goes while the process stays (back out
    // of the app): Dart, which owns the players, never disposed them.
    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
        destroyAll()
    }

    override fun onAttachedToActivity(binding: ActivityPluginBinding) {
        activity = binding.activity
        textures.values.forEach { it.setActivity(binding.activity) }
    }

    override fun onDetachedFromActivityForConfigChanges() = onDetachedFromActivity()

    override fun onReattachedToActivityForConfigChanges(binding: ActivityPluginBinding) =
        onAttachedToActivity(binding)

    override fun onDetachedFromActivity() {
        activity = null
        textures.values.forEach { it.setActivity(null) }
    }

    // Releases every texture and destroys its player.
    private fun destroyAll() {
        textures.values.forEach {
            it.release()
            LycheePlayer.destroyHandle(it.player)
        }
        textures.clear()
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "createTexture" -> {
                val player = call.argument<Number>("player")!!.toLong()
                val entry = textureRegistry.createSurfaceTexture()
                textures[entry.id()] =
                    LycheeTexture(entry, player, refreshRate()).also { it.setActivity(activity) }
                result.success(entry.id())
            }
            // After a hot restart: the previous isolate's players still play.
            "disposeAll" -> {
                destroyAll()
                result.success(null)
            }
            "setDisplayModeMatching" -> {
                val id = call.argument<Number>("textureId")!!.toLong()
                textures[id]?.displayModeMatching = call.argument<Boolean>("enabled")!!
                result.success(null)
            }
            "setSurfaceView" -> {
                val id = call.argument<Number>("textureId")!!.toLong()
                val rect = call.argument<List<Number>>("rect")?.map { it.toInt() }?.toIntArray()
                result.success(textures[id]?.setViewRect(rect) ?: false)
            }
            "disposeTexture" -> {
                val id = call.argument<Number>("textureId")!!.toLong()
                textures.remove(id)?.release()
                result.success(null)
            }
            else -> result.notImplemented()
        }
    }

    private fun refreshRate(): Float {
        val displays = context.getSystemService(DisplayManager::class.java)
        return displays.getDisplay(Display.DEFAULT_DISPLAY)?.refreshRate ?: 60f
    }
}
