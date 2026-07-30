#ifndef PUTORANA_GRAPHICS_FRAME_H
#define PUTORANA_GRAPHICS_FRAME_H

#include <cstdint>

/**
 * Free functions rather than a class, on purpose. Drawing a frame is the job of
 * the renderer, and the renderer is not one object: it is this whole namespace,
 * reaching into the device, the scene, the reconstruction, and whatever else
 * shows up. Wrapping that in a class named Renderer would only be a namespace
 * with worse ergonomics.
 * */
namespace putorana::graphics {

/**
 * One frame. Called from the render thread, once per vsync, driven by
 * Choreographer on the Kotlin side (RenderThread.kt).
 *
 * frameTimeNanos is the vsync timestamp Choreographer hands out, on the same
 * clock as System.nanoTime. It is the time the frame is *intended* to be
 * displayed, not the time this call started, which is why animation should be
 * driven from it rather than from a clock read in here — that is what keeps
 * motion smooth when a frame arrives late.
 * */
void DrawFrame(int64_t frameTimeNanos);

/**
 * Drops the frame loop's bookkeeping (timing stats, frame counters). Called when
 * the surface goes away, so the next surface starts from a clean slate instead
 * of measuring across the gap.
 * */
void ResetFrameStats();

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_FRAME_H
