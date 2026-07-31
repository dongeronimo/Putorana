package dev.dongeronimo.arreconstructor

import android.content.Context
import android.util.AttributeSet
import android.view.SurfaceHolder
import android.view.SurfaceView

/**
 * The window Vulkan draws into.
 *
 * SurfaceView is the right choice here: it gets its own compositor layer, backed
 * by a BufferQueue that SurfaceFlinger consumes directly, and that layer's
 * ANativeWindow is exactly what vkCreateAndroidSurfaceKHR wants. A TextureView
 * would work too but its buffers have to make a detour through the view
 * hierarchy's own rendering, which costs a copy per frame and adds latency.
 *
 * The view holds nothing but the render thread, and only for as long as there is
 * a surface to feed it. Everything that matters — surface, device, swapchain —
 * has a lifetime tied to the surface, not to the view or the Activity, so
 * putting any of it here would be tying it to the wrong thing.
 */
class VulkanSurfaceView @JvmOverloads constructor(
    context: Context,
    attrs: AttributeSet? = null,
    defStyleAttr: Int = 0,
) : SurfaceView(context, attrs, defStyleAttr), SurfaceHolder.Callback {

    private var renderThread: RenderThread? = null

    init {
        // Before the callback is registered, so it cannot lose a race with a
        // surface that already exists: the world is loaded from assets the
        // moment there is a device to load it onto.
        //
        // The application context's AssetManager and not this view's, because
        // native holds a global reference to it for the life of the process and
        // an Activity-scoped one would be pinned across configuration changes.
        NativeRenderer.setAssetManager(context.applicationContext.assets)
        holder.addCallback(this)
    }

    /**
     * The surface exists. Note that its size is not trustworthy yet — Android
     * always follows this with [surfaceChanged], which is where the real
     * dimensions show up and where the frame loop starts.
     */
    override fun surfaceCreated(holder: SurfaceHolder) {
        renderThread = RenderThread().apply { surfaceCreated(holder.surface) }
    }

    override fun surfaceChanged(holder: SurfaceHolder, format: Int, width: Int, height: Int) {
        renderThread?.surfaceChanged(format, width, height)
    }

    /**
     * Blocking, by way of [RenderThread.shutdown]. Returning from here tells
     * Android that nothing is using the surface any more, so the render thread
     * has to be genuinely finished with it — not merely told to stop.
     */
    override fun surfaceDestroyed(holder: SurfaceHolder) {
        renderThread?.shutdown()
        renderThread = null
    }
}
