package tech.soit.lychee.lychee_player_example

import io.flutter.embedding.android.FlutterActivity
import io.flutter.embedding.android.RenderMode

class MainActivity : FlutterActivity() {
    // LycheeVideo(surfaceView: true) puts its SurfaceView below Flutter's
    // view, which then has to be a TextureView.
    override fun getRenderMode(): RenderMode = RenderMode.texture
}
