package tech.soit.flutter.lychee.media_player

import android.os.Bundle
import io.flutter.embedding.android.FlutterActivity
import tech.soit.flutter.lychee.MediaPlayerBridge

class MainActivity : FlutterActivity() {

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)

        // flutterEngine created in super's onCreate
        MediaPlayerBridge.init(flutterEngine!!.renderer)
    }


}
