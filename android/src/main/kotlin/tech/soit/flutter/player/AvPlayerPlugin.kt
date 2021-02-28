package tech.soit.flutter.player

import androidx.annotation.NonNull
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result
import org.libsdl.app.SDL

/** AvPlayerPlugin */
class AvPlayerPlugin : FlutterPlugin, MethodCallHandler {

    override fun onAttachedToEngine(@NonNull flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        SDL.initialize();
        SDL.setContext(flutterPluginBinding.applicationContext);
        SDL.loadLibrary("SDL2")
        SDL.setupJNI()
        System.loadLibrary("media_player_android")
        MediaPlayerBridge.onAttached(flutterPluginBinding.textureRegistry)
    }

    override fun onMethodCall(@NonNull call: MethodCall, @NonNull result: Result) {

    }

    override fun onDetachedFromEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {

    }
}
