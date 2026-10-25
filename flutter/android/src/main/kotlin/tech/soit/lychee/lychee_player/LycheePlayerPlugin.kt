package tech.soit.lychee.lychee_player

import android.content.Context
import android.hardware.display.DisplayManager
import android.view.Display
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel
import io.flutter.view.TextureRegistry

class LycheePlayerPlugin : FlutterPlugin, MethodChannel.MethodCallHandler {
    private lateinit var channel: MethodChannel
    private lateinit var textureRegistry: TextureRegistry
    private lateinit var context: Context
    private val textures = HashMap<Long, LycheeTexture>()

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        // The core from the Android SDK, whose C ABI Dart opens through FFI;
        // loaded here so its FFmpeg dependencies resolve.
        System.loadLibrary("lychee")
        context = binding.applicationContext
        textureRegistry = binding.textureRegistry
        channel = MethodChannel(binding.binaryMessenger, "lychee_player")
        channel.setMethodCallHandler(this)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        channel.setMethodCallHandler(null)
        textures.values.forEach { it.release() }
        textures.clear()
    }

    override fun onMethodCall(call: MethodCall, result: MethodChannel.Result) {
        when (call.method) {
            "createTexture" -> {
                val player = call.argument<Number>("player")!!.toLong()
                val producer = textureRegistry.createSurfaceProducer()
                textures[producer.id()] = LycheeTexture(producer, player, refreshRate())
                result.success(producer.id())
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
