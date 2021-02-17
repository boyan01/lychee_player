package tech.soit.flutter.av_player

import androidx.annotation.NonNull
import io.flutter.embedding.engine.plugins.FlutterPlugin
import io.flutter.plugin.common.MethodCall
import io.flutter.plugin.common.MethodChannel.MethodCallHandler
import io.flutter.plugin.common.MethodChannel.Result

/** AvPlayerPlugin */
class AvPlayerPlugin : FlutterPlugin, MethodCallHandler {

    init {
        System.loadLibrary("av_player")
    }

    override fun onAttachedToEngine(@NonNull flutterPluginBinding: FlutterPlugin.FlutterPluginBinding) {

    }

    override fun onMethodCall(@NonNull call: MethodCall, @NonNull result: Result) {

    }

    override fun onDetachedFromEngine(@NonNull binding: FlutterPlugin.FlutterPluginBinding) {

    }
}
