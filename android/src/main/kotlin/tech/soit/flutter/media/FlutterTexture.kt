package tech.soit.flutter.media

import android.os.Handler
import android.os.Looper
import android.util.Log
import android.view.Surface
import androidx.annotation.Keep
import io.flutter.view.TextureRegistry

@Keep
class FlutterTexture(
    private val texture: TextureRegistry.SurfaceTextureEntry
) {

    companion object {
        private const val TAG = "FlutterTexture"
    }

    private val surface = Surface(texture.surfaceTexture())

    init {
        Log.i(TAG, "FlutterTexture init: texture_id = ${texture.id()}")
    }

    fun getId(): Long {
        return texture.id()
    }

    fun getSurface(): Surface {
        return surface
    }

    fun release() {
        surface.release()
        Handler(Looper.getMainLooper()).post {
            texture.release()
        }
    }

}