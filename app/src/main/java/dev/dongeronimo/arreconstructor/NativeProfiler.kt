package dev.dongeronimo.arreconstructor

/**
 * Reads the renderer's GPU timings.
 *
 * Separate from [NativeRenderer] because of the threading contract. Everything
 * on NativeRenderer must be called from the render thread, and the native side
 * relies on that to take no locks at all. This one is the exception: it is read
 * from the UI thread, to drive an overlay, while the render thread is writing
 * the next frame's numbers. GpuProfiler takes a lock precisely so it can be.
 */
object NativeProfiler {

    init {
        // Idempotent, like the ones in NativeRenderer and NativeAr.
        System.loadLibrary("arreconstructor")
    }

    /**
     * One entry per render pass that declared a scope, in the order they were
     * recorded — so an enclosing scope comes before the passes inside it.
     *
     * Empty is normal and not an error: before the first frames have retired,
     * while there is no surface, and forever on a device whose queue family does
     * not support timestamp queries.
     */
    external fun passTimings(): Array<PassTiming>
}
