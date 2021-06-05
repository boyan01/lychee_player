package com.github.boyan01.lychee

import io.flutter.embedding.engine.plugins.FlutterPlugin

class LycheePlayerPlugin : FlutterPlugin {

    override fun onAttachedToEngine(binding: FlutterPlugin.FlutterPluginBinding) {
        MediaPlayerBridge.init(binding.textureRegistry)
    }

    override fun onDetachedFromEngine(binding: FlutterPlugin.FlutterPluginBinding) {

    }

}