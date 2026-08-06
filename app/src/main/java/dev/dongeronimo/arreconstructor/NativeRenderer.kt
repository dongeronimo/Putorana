package dev.dongeronimo.arreconstructor

import android.content.res.AssetManager
import android.view.Surface

/**
 * The Kotlin side of the native renderer: the entry points into
 * putorana::graphics. Nothing here holds state, and nothing here does its own
 * threading — every one of these must be called from the render thread, which is
 * [RenderThread]'s job. The native side relies on that and takes no locks.
 */
object NativeRenderer {

    init {
        // System.loadLibrary is process wide and idempotent, so it does not
        // matter whether MainActivity or this object gets initialized first:
        // whoever is first actually loads the library (and therefore runs
        // JNI_OnLoad, which creates the VkInstance), and the second call is a
        // no-op. Declaring it in both places means neither one has to know it is
        // being used before the other.
        System.loadLibrary("arreconstructor")
    }

    /**
     * Hands the native side the [AssetManager] it reads models and shaders
     * through. Must be called before the first [surfaceCreated], since the world
     * is built from assets as soon as there is a device to build it on.
     *
     * An app's assets are not files: they live inside the APK, usually
     * compressed, so there is no path native code could open. Native keeps a
     * global JNI reference to [assets] — the AAssetManager pointer it derives is
     * only valid while the Java object is alive.
     *
     * Unlike everything else here, this one does NOT have to be on the render
     * thread: it only stores a pointer, and it runs before that thread exists.
     */
    external fun setAssetManager(assets: AssetManager)

    /**
     * The window now exists and can be rendered to. [surface] is only borrowed
     * for the duration of this call: native code turns it into an ANativeWindow,
     * which takes its own reference, and releases that in [surfaceDestroyed].
     *
     * Per README > Graphics Architecture this is where VkSurfaceKHR, the
     * physical device and the logical device get created.
     */
    external fun surfaceCreated(surface: Surface)

    /**
     * The window changed size or pixel format. Always called at least once right
     * after [surfaceCreated], so this — not surfaceCreated — is the reliable
     * place to learn the actual dimensions, and where the swapchain is
     * created/recreated.
     *
     * [format] is an android.graphics.PixelFormat constant. Vulkan does not use
     * it (the swapchain negotiates its own format through
     * vkGetPhysicalDeviceSurfaceFormatsKHR) but it is passed through so the
     * native side can tell a real resize from a format-only change.
     */
    external fun surfaceChanged(format: Int, width: Int, height: Int)

    /**
     * The window is going away. This must fully tear down anything that touches
     * it before returning — vkDeviceWaitIdle, destroy swapchain, destroy
     * VkSurfaceKHR, release the ANativeWindow. Android's contract is that the
     * Surface is invalid the instant this returns, and using it afterwards is
     * undefined behaviour, not an exception.
     */
    external fun surfaceDestroyed()

    /**
     * Draws one frame. Called once per vsync from RenderThread's Choreographer
     * callback, and only between [surfaceChanged] and [surfaceDestroyed].
     *
     * [frameTimeNanos] is the vsync timestamp, on the same clock as
     * System.nanoTime. It is when the frame is meant to be *displayed*, not when
     * this call started, so animation driven from it stays smooth even when a
     * frame is produced late.
     */
    external fun drawFrame(frameTimeNanos: Long)

    /**
     * Switches the scene. [worldId] is [WorldId.nativeId]; an id the renderer
     * does not know is logged and ignored, leaving the current scene up.
     *
     * Asynchronous in the only sense that matters: this records the request and
     * returns, and the teardown of the old world and the build of the new one
     * happen inside the next [drawFrame]. That is not an optimisation — a world
     * builds its pipelines against the swapchain's format, and the frame loop is
     * the only thing that knows there is a swapchain.
     *
     * Asking for the world already showing rebuilds it, which is the cheapest
     * way to exercise the load/unload path without backgrounding the app.
     */
    external fun setWorld(worldId: Int)
}
