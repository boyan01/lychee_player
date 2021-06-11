package com.github.boyan01.lychee

import android.os.Handler
import android.os.Looper
import android.util.Log
import androidx.annotation.Keep
import io.flutter.view.TextureRegistry
import java.util.concurrent.CountDownLatch

@Keep
object MediaPlayerBridge {

    init {
        System.loadLibrary("lychee_player")
    }

    private const val TAG = "MediaPlayerBridge"

    private var textureRegistry: TextureRegistry? = null

    internal fun init(textureRegistry: TextureRegistry) {
        if (MediaPlayerBridge.textureRegistry != null) {
            throw RuntimeException("texture registry already registered.")
        }
        MediaPlayerBridge.textureRegistry = textureRegistry
        setupJNI()
    }


    @Keep
    @JvmStatic
    fun register(): FlutterTexture? {
        val countDownLatch = CountDownLatch(1)

        var texture: TextureRegistry.SurfaceTextureEntry? = null
        Handler(Looper.getMainLooper()).post {
            texture = textureRegistry?.createSurfaceTexture()
            countDownLatch.countDown()
        }
        countDownLatch.await()
        Log.d(TAG, "register: $texture")
        texture ?: return null
        return FlutterTexture(texture!!)
    }

    @Keep
    @JvmStatic
    private external fun setupJNI();


}