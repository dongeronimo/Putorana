#ifndef PUTORANA_GRAPHICS_DEVICE_H
#define PUTORANA_GRAPHICS_DEVICE_H

#include "Allocator.h"
#include "DescriptorPool.h"
#include "FrameRing.h"
#include "GpuProfiler.h"
#include "PhysicalDevice.h"
#include "Swapchain.h"
#include "volk.h"

#include <android/native_window.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace putorana::graphics {

// Forward declared rather than included: World sits far above this class and
// pulls in the whole scene graph. Device.cpp includes it, which is where the
// unique_ptr below needs the complete type.
class World;

/**
 * Everything whose lifetime follows the Android surface: the ANativeWindow, the
 * VkSurfaceKHR built on it and, next, the VkPhysicalDevice, the VkDevice with
 * its queues, and the swapchain hanging off them. The VkInstance is deliberately
 * not here — it outlives all of this, see Instance.h.
 *
 * This is not the renderer. The renderer is whatever draws *using* a device, and
 * it is not a class at all: it is this namespace. Device is only the bundle of
 * resources it draws with, which is why DrawFrame lives in Frame.h and not here.
 *
 * The On* methods are the native half of SurfaceHolder.Callback. They do not run
 * on the UI thread: RenderThread.kt hops them onto the render thread first, so
 * everything in this class happens on that single thread and needs no locking.
 *
 * There is exactly ONE exception, and it is SnapshotTimings(). The debug overlay
 * polls GPU timings from the UI thread while the render thread keeps rendering —
 * that is the entire point of it — so that one path is synchronised and every
 * other member is not. Anything else reached from another thread has to be added
 * to this comment and given the same treatment, because the default here is no
 * locking at all.
 * */
class Device {
public:
    Device() = default;
    ~Device();

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;

    /**
     * Takes ownership of a window already acquired with
     * ANativeWindow_fromSurface and builds the VkSurfaceKHR on it. Passing null
     * is tolerated and logged, because that is what a Surface that died between
     * the Java callback and this call looks like.
     *
     * All or nothing: if anything fails the window is released again and the
     * object is left empty, so HasSurface() is the single thing callers have to
     * check.
     * */
    void OnSurfaceCreated(ANativeWindow* window);

    /**
     * New size or new pixel format. Always follows OnSurfaceCreated at least
     * once, which is why the swapchain is invalidated from here rather than
     * built there.
     * */
    void OnSurfaceChanged(int32_t width, int32_t height);

    /**
     * Must leave nothing pointing at the window when it returns: the UI thread
     * is blocked on this and Android invalidates the Surface right after.
     * */
    void OnSurfaceDestroyed();

    /**
     * True once the whole chain is up: window, surface, adapter and logical
     * device. There is no partial state — OnSurfaceCreated unwinds everything it
     * built if any step fails, so this one check covers all of them.
     * */
    bool HasSurface() const { return device_ != VK_NULL_HANDLE; }

    /**
     * Valid only while HasSurface(). The physical device query, the swapchain
     * and every present call go through this.
     * */
    VkSurfaceKHR surface() const { return surface_; }

    VkDevice handle() const { return device_; }

    /** Valid only while HasSurface(). */
    const PhysicalDevice& physicalDevice() const { return physicalDevice_.value(); }

    /**
     * The single queue, from the single family. Graphics, compute, transfer and
     * present all go here — see PhysicalDevice::queueFamily().
     * */
    VkQueue queue() const { return queue_; }

    /**
     * Marks the swapchain as no longer matching the window. Two callers: a
     * resize coming down from Android through OnSurfaceChanged, and the frame
     * loop itself when vkAcquireNextImageKHR or vkQueuePresentKHR answers
     * VK_ERROR_OUT_OF_DATE_KHR or VK_SUBOPTIMAL_KHR. The second path is the one
     * that fires on rotation, and it is the only one on devices where the
     * compositor changes the transform without changing the window size.
     * */
    void InvalidateSwapchain() { swapchainOutOfDate_ = true; }

    /**
     * Rebuilds the swapchain when it is stale, and builds the first one. Returns
     * false when there is nothing to render into, in which case the caller must
     * skip the frame and try again on the next one — a window mid-resize
     * legitimately has zero area for a moment.
     * */
    bool RecreateSwapchainIfNeeded();

    /** Valid only after RecreateSwapchainIfNeeded() has returned true. */
    const Swapchain& swapchain() const { return *swapchain_; }

    /**
     * Valid whenever HasSurface(). Device lifetime, not swapchain lifetime — a
     * resize does not touch it.
     * */
    FrameRing& frames() const { return *frames_; }

    /**
     * Where every VkBuffer and VkImage comes from. Valid whenever HasSurface(),
     * and only then — it is destroyed with the VkDevice, so anything holding an
     * allocation has to be released before the surface goes away. Pressing Home
     * runs that teardown, so "load the meshes once at startup" is not an option
     * in this app: whatever owns them has to be rebuilt with the device.
     * */
    const Allocator& allocator() const { return *allocator_; }

    /**
     * Where materials and render passes get their descriptor sets. Device
     * lifetime for the same reason as the allocator: a VkDescriptorSet belongs
     * to a pool that belongs to a VkDevice, and all three go at once.
     * */
    DescriptorPool& descriptorPool() const { return *descriptorPool_; }

    /**
     * GPU timing, for the debug overlay. Device lifetime — its query pools
     * belong to the VkDevice — so the Kotlin side must tolerate it going away
     * and coming back when the app is backgrounded.
     *
     * RENDER THREAD ONLY, and it is not null-checked. That thread is the one
     * that creates and destroys the profiler, so it is the only one that can
     * know the pointer is alive for the duration of a frame. Anyone else must
     * use SnapshotTimings().
     * */
    GpuProfiler& profiler() const { return *profiler_; }

    /**
     * The timings, safe to call from any thread at any moment. Empty when there
     * is no profiler right now.
     *
     * This exists because HasSurface() is NOT a valid guard for touching the
     * profiler, which is what the UI thread used to do. The two are not created
     * or destroyed together: OnSurfaceDestroyed frees the profiler and only
     * nulls device_ eleven lines and a vkDestroyDevice later, and on the way up
     * device_ is set before GpuProfiler::Create returns. Both leave a window
     * where HasSurface() is true and the profiler is not there — which crashed
     * inside Snapshot(), on the lock of a mutex that had been freed.
     *
     * So the check and the use have to happen under one lock, which means they
     * have to happen in one function, which is this one.
     * */
    std::vector<PassTiming> SnapshotTimings() const;

    /**
     * The scene being drawn, or null before one is installed.
     *
     * Device owns it for one reason: a world's meshes are allocations from the
     * allocator below, so the world MUST be gone before that allocator is. Any
     * other owner turns that into a rule somebody has to remember on a teardown
     * path that runs every time the app is backgrounded. Owned here, the
     * ordering is simply what the destructor does.
     * */
    World* world() const { return world_.get(); }

    /**
     * Installs the scene, destroying whatever was there. Call once there is a
     * swapchain — a world builds its pipelines against the surface format, so it
     * cannot be constructed any earlier.
     * */
    void SetWorld(std::unique_ptr<World> world);

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }

private:
    /**
     * Selects the adapter and builds the logical device on top of an already
     * created surface. Returns false with the reason logged; the caller unwinds.
     * */
    bool CreateLogicalDevice();

    ANativeWindow* window_ = nullptr;

    /**
     * The instance the surface was created from, cached rather than looked up
     * again at teardown. Vulkan requires vkDestroySurfaceKHR to be handed the
     * same instance that created it, so keeping the pair together in one object
     * is the only way it cannot drift.
     * */
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;

    std::optional<PhysicalDevice> physicalDevice_;
    VkDevice device_ = VK_NULL_HANDLE;
    VkQueue queue_ = VK_NULL_HANDLE;
    std::unique_ptr<Allocator> allocator_;
    std::unique_ptr<DescriptorPool> descriptorPool_;
    std::unique_ptr<FrameRing> frames_;
    std::unique_ptr<GpuProfiler> profiler_;
    /**
     * Guards the profiler_ POINTER, not the object — GpuProfiler has its own
     * lock for its contents. Held only where profiler_ is assigned or reset, and
     * in SnapshotTimings(); the render thread's own profiler() calls stay
     * unlocked because that thread is the one holding the object alive.
     * */
    mutable std::mutex profilerMutex_;
    std::unique_ptr<Swapchain> swapchain_;
    /** Declared last, so it is destroyed first — before the allocator it drew from. */
    std::unique_ptr<World> world_;

    int32_t width_ = 0;
    int32_t height_ = 0;
    bool swapchainOutOfDate_ = false;
};

/**
 * Same shape as instance_holder in Instance.h and for the same reason: the JNI
 * entry points are free functions with no object to hang off of, so the device
 * has to be reachable by name.
 * */
namespace device_holder {

/** The one device. Constructed on first use, empty until a surface arrives. */
Device& Get();

} // namespace device_holder

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_DEVICE_H
