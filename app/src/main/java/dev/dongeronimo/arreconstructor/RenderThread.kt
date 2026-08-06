package dev.dongeronimo.arreconstructor

import android.os.Handler
import android.os.HandlerThread
import android.view.Choreographer
import android.view.Surface
import java.util.concurrent.CountDownLatch

/**
 * Drives the renderer. Owns one thread; every native call in this app that is
 * not the instance report happens on it, which is what lets the C++ side skip
 * locking entirely.
 *
 * The thread's life is exactly the surface's life: it is started by the
 * constructor when the surface appears and joined by [shutdown] when it goes
 * away. Everything Vulkan gets rebuilt on a new surface anyway, so keeping the
 * thread alive across the gap would buy nothing and only leave stale state
 * around to reason about.
 *
 * Frames come from [Choreographer], not from a `while (true)` loop. Choreographer
 * fires once per vsync on the Looper it was obtained on — the render thread's,
 * here — so the loop cannot outrun the display, and each callback carries the
 * timestamp of the vsync it belongs to.
 */
class RenderThread {

    private val thread = HandlerThread("PutoranaRender")
    private val handler: Handler

    /** Render thread only. */
    private var choreographer: Choreographer? = null

    /** Render thread only. False between shutdown's first act and the join. */
    private var frameLoopRunning = false

    private val frameCallback = object : Choreographer.FrameCallback {
        override fun doFrame(frameTimeNanos: Long) {
            if (!frameLoopRunning) {
                return
            }
            // Ask for the next vsync *before* drawing. If this frame overruns
            // its budget the request is already in, so the loop resumes on the
            // next vsync instead of losing one to the round trip.
            choreographer?.postFrameCallback(this)
            NativeRenderer.drawFrame(frameTimeNanos)
        }
    }

    init {
        thread.start()
        // HandlerThread.looper blocks until the Looper is actually up, so the
        // Handler below is always valid.
        handler = Handler(thread.looper)
        // Choreographer.getInstance() is per-thread and needs a Looper, so it
        // has to be fetched from the render thread itself.
        handler.post { choreographer = Choreographer.getInstance() }
    }

    /**
     * Hands the window to native code. Asynchronous: the UI thread has no reason
     * to wait for a device to be built. [surface] stays valid long enough
     * because [shutdown] is the only thing that invalidates it and it blocks on
     * this same queue, so this always runs first.
     */
    fun surfaceCreated(surface: Surface) {
        handler.post { NativeRenderer.surfaceCreated(surface) }
    }

    /**
     * Reports a resize (or a format change) and starts the frame loop if it is
     * not running yet. Android always sends surfaceChanged right after
     * surfaceCreated, so this — not surfaceCreated — is where the loop begins,
     * and it is also where it restarts after every rotation.
     */
    fun surfaceChanged(format: Int, width: Int, height: Int) {
        handler.post {
            NativeRenderer.surfaceChanged(format, width, height)
            if (!frameLoopRunning) {
                frameLoopRunning = true
                choreographer?.postFrameCallback(frameCallback)
            }
        }
    }

    /**
     * Asks the renderer for a different world. Posted rather than called
     * straight through, because the caller is the UI thread — a menu tap — and
     * the native side takes no locks precisely because everything reaches it on
     * this one thread.
     *
     * Fire and forget: building a world reads assets and compiles pipelines, and
     * there is nothing the UI would do with the outcome that logcat does not
     * already say better.
     */
    fun setWorld(worldId: Int) {
        handler.post { NativeRenderer.setWorld(worldId) }
    }

    /**
     * Stops the loop, tears the native side down and joins the thread. Blocks,
     * because the caller is SurfaceHolder.Callback.surfaceDestroyed and Android
     * invalidates the Surface the moment that returns: anything still touching
     * the window after that point is a use-after-free inside the driver, not an
     * exception.
     *
     * There is no timeout on the wait on purpose. If the native teardown hangs,
     * an ANR with the render thread's stack in the trace is a far better bug
     * report than a silent race.
     */
    fun shutdown() {
        val finished = CountDownLatch(1)
        handler.post {
            frameLoopRunning = false
            choreographer?.removeFrameCallback(frameCallback)
            NativeRenderer.surfaceDestroyed()
            finished.countDown()
        }
        finished.await()

        thread.quitSafely()
        thread.join()
    }
}
