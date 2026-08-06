#include "WorldRegistry.h"

#include "graphics/Device.h"
#include "graphics/World.h"
#include "worlds/helloWorld/HelloWorld.h"
#include "worlds/openChiselWorld/OpenChiselWorld.h"

#include <android/log.h>

#include <memory>
#include <string>
#include <utility>

namespace putorana::worlds {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * What the menu last asked for, and what is actually installed.
 *
 * Render thread only, so plain globals are enough, the same reasoning as the
 * frame loop's own statics in Frame.cpp.
 *
 * `g_requested` deliberately outlives the Device. A trip to background destroys
 * the world and rebuilds it from here, so the app comes back showing what it was
 * showing, and the Kotlin side never has to re-send anything.
 * */
WorldKind g_requested = WorldKind::Hello;

/**
 * Set once a build has been ATTEMPTED for g_requested against the current
 * surface, whether or not it worked.
 *
 * Without it a world that fails to build is retried sixty times a second: sixty
 * identical stack traces in logcat, and sixty rounds of allocating and releasing
 * whatever it got through before failing. The cost is that a world which failed
 * only gets another go once something else is picked, or once the surface
 * cycles. That is the right trade for a failure that is almost always a shader
 * or an asset, and so will fail again.
 * */
bool g_attempted = false;

const char* Name(WorldKind kind) {
    switch (kind) {
        case WorldKind::Hello:
            return "HelloWorld";
        case WorldKind::Behaviours:
            return "Behaviours";
        case WorldKind::OpenChisel:
            return "OpenChisel";
    }
    return "unknown";
}

/**
 * The one place in the app that names a concrete world. Everything else (the
 * frame loop, the Device, the JNI layer) only knows the World interface, so
 * adding a world is an entry in WorldKind, a case here, and nothing else.
 *
 * Null means "no such world", which is a normal answer and not an error: see
 * WorldKind::Behaviours.
 *
 * Construction only. It allocates nothing on the GPU (see World.h on the
 * two-phase build) so calling it costs nothing when the answer is thrown away.
 * */
std::unique_ptr<graphics::World> Make(WorldKind kind, graphics::Device& device) {
    switch (kind) {
        case WorldKind::Hello:
            return std::make_unique<hello::HelloWorld>(device);
        case WorldKind::OpenChisel:
            return std::make_unique<openChisel::OpenChiselWorld>(device);
        case WorldKind::Behaviours:
            // Reserved, not built. See WorldKind::Behaviours.
            return nullptr;
    }
    return nullptr;
}

} // namespace

void Request(int32_t nativeId) {
    const auto kind = static_cast<WorldKind>(nativeId);
    switch (kind) {
        case WorldKind::Hello:
        case WorldKind::Behaviours:
        case WorldKind::OpenChisel:
            break;
        default:
            __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                                "world %d is not a WorldKind; staying on %s", nativeId,
                                Name(g_requested));
            return;
    }

    g_requested = kind;
    // Even when the kind has not changed. Re-picking rebuilds, on purpose; see
    // the header.
    g_attempted = false;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "world requested: %s", Name(kind));
}

void InstallIfNeeded(graphics::Device& device) {
    if (g_attempted) {
        return;
    }
    g_attempted = true;

    std::unique_ptr<graphics::World> world = Make(g_requested, device);
    if (world == nullptr) {
        // Nothing has been torn down yet, which is the point of building before
        // destroying: an unimplemented world costs the scene on screen nothing.
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "no native world for %s; keeping the current scene", Name(g_requested));
        return;
    }

    // Whatever is showing goes NOW, before the incoming world's passes and
    // assets allocate anything.
    //
    // Two worlds alive at once is a real peak, not a theoretical one: an
    // OpenChisel session holds one mutable mesh per chunk of the room and has
    // already taken this process out of memory twice. And keeping the old world
    // as a fallback would be a fiction: its render targets were built for this
    // swapchain and there is no path that puts it back.
    //
    // SetWorld waits for the GPU to go idle first, so the buffers a frame in
    // flight is still reading are not freed underneath it.
    device.SetWorld(nullptr);

    std::string error;
    if (!world->CreateRenderPasses(device.swapchain(), error)) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s render passes failed: %s",
                            Name(g_requested), error.c_str());
        return;
    }
    if (!world->CreateWorld(error)) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s content failed: %s", Name(g_requested),
                            error.c_str());
        return;
    }
    device.SetWorld(std::move(world));
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "world installed: %s", Name(g_requested));
}

void OnSurfaceLost() {
    // The world went with the Device, so the next surface gets a fresh attempt.
    // This is the "no loading once at startup" rule made concrete: the whole
    // scene is rebuilt every time the app comes back from background.
    g_attempted = false;
}

} // namespace putorana::worlds
