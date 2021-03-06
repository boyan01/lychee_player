package tech.soit.flutter.player

import androidx.annotation.NonNull
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result

/** AvPlayerPlugin */
class AvPlayerPlugin : FlutterPlugin, MethodCallHandler {

    override fun onAttachedToEngine(@NonNull flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {
        System.loadLibrary("media_player_android")
        MediaPlayerBridge.onAttached(flutterPluginBinding.textureRegistry)
    }

    override fun onMethodCall(@NonNull call: MethodCall, @NonNull result: Result) {

    }

    override fun onDetachedFromEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {

    }
}
