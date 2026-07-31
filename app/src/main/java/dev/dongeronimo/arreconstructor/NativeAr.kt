package dev.dongeronimo.arreconstructor

import android.content.Context
import android.view.Surface

/**
 * The Kotlin side of putorana::ar::Subsystem — ARCore, and nothing to do with
 * Vulkan.
 *
 * Unlike [NativeRenderer], these are called from the UI thread: the session's
 * life follows the ACTIVITY, not the surface. That distinction is the whole
 * reason this is a separate object. Destroying an ArSession throws away tracking
 * and every anchor with it, so it must not be torn down and rebuilt every time
 * the surface goes away — which is exactly what happens to the device, the
 * swapchain and the world on any rotation.
 *
 * The native side takes a lock, because [create]/[resume]/[pause] land here on
 * the UI thread while the render thread is ticking the same session once per
 * frame.
 */
object NativeAr {

    init {
        // Idempotent, like the one in NativeRenderer. Whichever object is
        // touched first loads the library.
        System.loadLibrary("arreconstructor")
    }

    // The last geometry reported, remembered because the two things that
    // determine it arrive in an order nobody controls.
    //
    // With the permission already granted, the session is created in onCreate and
    // the surface appears afterwards. With the dialog shown, the grant lands long
    // after the first surfaceChanged, so the geometry is reported to a session
    // that does not exist yet and is silently dropped — and nothing would ever
    // report it again, because surfaceChanged does not fire twice for the same
    // surface. The symptom is a camera image rotated ninety degrees, but only on
    // the very first run after an install.
    private var lastRotation = Surface.ROTATION_0
    private var lastWidth = 0
    private var lastHeight = 0

    /**
     * Creates the session. Call once, after the CAMERA permission has been
     * granted — ARCore does not prompt, and creating without it only defers the
     * failure to [resume].
     *
     * [context] should be the Activity: it is what ARCore's install flow needs
     * if the ARCore APK turns out to be missing or too old.
     *
     * Reports failure rather than throwing. The renderer copes — no session
     * means no camera background, and the cube still draws.
     */
    fun create(context: Context): NativeSelfTestResult {
        val result = nativeCreate(context)
        if (result.ok && lastWidth > 0) {
            nativeSetDisplayGeometry(lastRotation, lastWidth, lastHeight)
        }
        return result
    }

    /**
     * The viewport ARCore is projecting into.
     *
     * [rotation] is the DISPLAY rotation, one of the Surface.ROTATION_*
     * constants — not the swapchain's preTransform, which is a related but
     * different quantity and would be wrong here on some devices. Without this,
     * the camera background comes out rotated or stretched, because the mapping
     * from sensor to screen is exactly what it feeds.
     *
     * Safe to call before the session exists; it is replayed by [create].
     */
    fun setDisplayGeometry(rotation: Int, width: Int, height: Int) {
        lastRotation = rotation
        lastWidth = width
        lastHeight = height
        nativeSetDisplayGeometry(rotation, width, height)
    }

    /** Starts the camera. From onResume, after [create] succeeded. */
    external fun resume(): NativeSelfTestResult

    /**
     * Stops the camera. From onPause, and not optional: a session left running
     * holds the camera open while the app is in the background, which Android
     * will eventually take away from it anyway — noisily.
     */
    external fun pause()

    private external fun nativeCreate(context: Context): NativeSelfTestResult

    private external fun nativeSetDisplayGeometry(rotation: Int, width: Int, height: Int)
}
