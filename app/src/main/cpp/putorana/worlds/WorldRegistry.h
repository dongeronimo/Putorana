#ifndef ARRECONSTRUCTOR_WORLDREGISTRY_H
#define ARRECONSTRUCTOR_WORLDREGISTRY_H

#include <cstdint>

namespace putorana::graphics {
class Device;
}

namespace putorana::worlds {

/**
 * Which world the app can show. The values ARE the wire format: they must match
 * WorldId.nativeId in WorldId.kt one for one, because that enum is what the
 * menu sends over JNI.
 *
 * Explicit numbers rather than the Kotlin enum's ordinal, so reordering the menu
 * cannot silently switch to the wrong scene — which is the failure the WorldId
 * comment was already worrying about.
 * */
enum class WorldKind : int32_t {
    Hello = 0,

    /**
     * Not written yet. Requesting it is refused rather than obeyed: tearing the
     * current scene down for a world that cannot be built would trade a working
     * picture for the placeholder clear, and the menu entry exists to reserve
     * the slot, not to blank the screen.
     * */
    Behaviours = 1,

    OpenChisel = 2,
};

/**
 * Asks for a world. Takes effect on the next frame that has a swapchain, which
 * is where the old one is destroyed and the new one built.
 *
 * RENDER THREAD ONLY, like everything else the renderer exposes — RenderThread.kt
 * hops the menu's selection onto it. Nothing here is locked.
 *
 * Asking for the world already showing REBUILDS it. That is deliberate: this is
 * the seam the load/unload path is tested through, and "tap the current entry to
 * reload it" is the cheapest way to exercise a teardown without backgrounding
 * the app.
 *
 * `nativeId` comes straight from Kotlin. One that matches no WorldKind is
 * logged and ignored, leaving whatever is on screen exactly where it was.
 * */
void Request(int32_t nativeId);

/**
 * Builds the requested world if it is not the one already installed, and builds
 * the first one of all.
 *
 * Called once per frame from the frame loop, AFTER the swapchain has been
 * confirmed — a world builds its pipelines against the surface format, so there
 * is nothing to build against before that.
 *
 * Does nothing on the frames in between, which is almost all of them.
 * */
void InstallIfNeeded(graphics::Device& device);

/**
 * The surface, and with it the Device and the world, went away.
 *
 * Only clears the "already tried" latch; the REQUESTED world survives, so an app
 * that was showing OpenChisel when it went to background is still showing it on
 * the way back. That is the whole reason this state lives here rather than in
 * the Device it outlives.
 * */
void OnSurfaceLost();

} // namespace putorana::worlds

#endif //ARRECONSTRUCTOR_WORLDREGISTRY_H
