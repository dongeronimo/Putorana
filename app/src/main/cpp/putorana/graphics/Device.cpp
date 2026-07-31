#include "Device.h"

#include "Instance.h"
// Needed here and not in the header: the unique_ptr<World> member cannot be
// destroyed without the complete type, and both destructors live in this file.
#include "World.h"

#include <android/log.h>

#include <string>
#include <vector>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

// Only the codes vkCreateAndroidSurfaceKHR can actually return. Instance.cpp has
// its own copy of this covering a different set; when a third caller needs one,
// that is the moment to pull them into a shared helper rather than now.
const char* SurfaceResultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS:
            return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY:
            return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY:
            return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_NATIVE_WINDOW_IN_USE_KHR:
            // The window already has a consumer attached — another VkSurfaceKHR
            // that was never destroyed, or an EGL/media surface on it.
            return "VK_ERROR_NATIVE_WINDOW_IN_USE_KHR";
        default:
            return "unexpected VkResult";
    }
}

} // namespace

Device::~Device() {
    OnSurfaceDestroyed();
}

void Device::SetWorld(std::unique_ptr<World> world) {
    if (world_ != nullptr) {
        // The outgoing world's buffers may still be read by a frame in flight.
        // Nothing else here can know that, so wait before letting it go.
        vkDeviceWaitIdle(device_);
        world_.reset();
    }
    world_ = std::move(world);
}

void Device::OnSurfaceCreated(ANativeWindow* window) {
    if (window == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "surfaceCreated: ANativeWindow_fromSurface returned null");
        return;
    }

    // Android sends destroyed before a second created, so this should not
    // happen. A silently leaked window would stay invisible for a long time
    // though, so drop whatever is here and say so.
    if (window_ != nullptr) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "surfaceCreated while a window was still held; releasing the old one");
        OnSurfaceDestroyed();
    }

    const Instance* instance = instance_holder::Get();
    if (instance == nullptr) {
        // JNI_OnLoad failed to build the instance. Nothing can be done with this
        // window, so give it straight back instead of holding a useless
        // reference until the surface is destroyed.
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "surfaceCreated: no VkInstance, nothing can be built on this window");
        ANativeWindow_release(window);
        return;
    }

    window_ = window;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "surfaceCreated: window is %dx%d",
                        ANativeWindow_getWidth(window_), ANativeWindow_getHeight(window_));

    // The surface is a thin wrapper: it hands the window to the driver as a
    // buffer consumer and answers questions about what can be presented to it.
    // It does not allocate images and it does not care about size — that is the
    // swapchain's job. Note the window is *not* dup'd here: the surface borrows
    // the reference this object holds, which is why the release at teardown has
    // to come after vkDestroySurfaceKHR.
    VkAndroidSurfaceCreateInfoKHR surfaceInfo{};
    surfaceInfo.sType = VK_STRUCTURE_TYPE_ANDROID_SURFACE_CREATE_INFO_KHR;
    surfaceInfo.window = window_;

    const VkResult result =
            vkCreateAndroidSurfaceKHR(instance->handle(), &surfaceInfo, nullptr, &surface_);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "vkCreateAndroidSurfaceKHR failed: %s (%d)",
                            SurfaceResultName(result), result);
        // Leave nothing half-built: HasSurface() is the only thing callers
        // check, so an object with a window but no surface must not exist.
        surface_ = VK_NULL_HANDLE;
        ANativeWindow_release(window_);
        window_ = nullptr;
        return;
    }

    instance_ = instance->handle();
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "VkSurfaceKHR created");

    if (!CreateLogicalDevice()) {
        OnSurfaceDestroyed();
        return;
    }

    swapchainOutOfDate_ = true;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "device ready\n%s",
                        physicalDevice_->Describe().c_str());
}

bool Device::CreateLogicalDevice() {
    std::string error;
    physicalDevice_ = PhysicalDevice::Select(instance_, surface_, error);
    if (!physicalDevice_.has_value()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return false;
    }

    const float queuePriority = 1.0f;
    VkDeviceQueueCreateInfo queueInfo{};
    queueInfo.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO;
    queueInfo.queueFamilyIndex = physicalDevice_->queueFamily();
    queueInfo.queueCount = 1;
    queueInfo.pQueuePriorities = &queuePriority;

    // Every one of these is core in 1.2/1.3, so none of them appear as extension
    // strings — they are turned on through the feature chain instead. Select()
    // already proved the adapter supports the whole set, so vkCreateDevice can
    // only fail here for reasons that have nothing to do with features.
    const RequiredFeatures features;
    const std::vector<const char*> extensions = RequiredDeviceExtensions();

    VkDeviceCreateInfo deviceInfo{};
    deviceInfo.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO;
    deviceInfo.pNext = features.chain();
    deviceInfo.queueCreateInfoCount = 1;
    deviceInfo.pQueueCreateInfos = &queueInfo;
    deviceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    deviceInfo.ppEnabledExtensionNames = extensions.data();
    // Must stay null. Chaining a VkPhysicalDeviceFeatures2 into pNext and also
    // setting pEnabledFeatures is invalid usage, and it is the single easiest
    // way to get a driver to reject a device that is otherwise correct.
    deviceInfo.pEnabledFeatures = nullptr;
    // enabledLayerCount / ppEnabledLayerNames are ignored: device layers were
    // deprecated in Vulkan 1.0.13. The validation layer is an instance layer and
    // it wraps this device anyway.

    const VkResult result =
            vkCreateDevice(physicalDevice_->handle(), &deviceInfo, nullptr, &device_);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "vkCreateDevice failed: %d", result);
        device_ = VK_NULL_HANDLE;
        physicalDevice_.reset();
        return false;
    }

    // volk's device-level pointers are still null at this point: Instance used
    // volkLoadInstanceOnly. Loading here swaps the whole global vk* table over
    // to this device's direct entry points, skipping the loader trampoline.
    // Two consequences worth knowing. The pointers dangle between
    // vkDestroyDevice and the next volkLoadDevice, which is safe only because
    // nothing renders while there is no surface; and vulkan_check.cpp uses a
    // local VolkDeviceTable precisely so its throwaway device never overwrites
    // them.
    volkLoadDevice(device_);

    // Device-level, so it could not have been called before volkLoadDevice.
    vkGetDeviceQueue(device_, physicalDevice_->queueFamily(), 0, &queue_);

    // After volkLoadDevice and not before: VMA is built with
    // VMA_DYNAMIC_VULKAN_FUNCTIONS and resolves its entry points through the
    // proc-address functions volk has just wired up.
    allocator_ = Allocator::Create(instance_, physicalDevice_->handle(), device_, error);
    if (allocator_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return false;
    }
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "allocator: %s",
                        allocator_->Describe().c_str());

    descriptorPool_ = DescriptorPool::Create(device_, error);
    if (descriptorPool_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return false;
    }

    frames_ = FrameRing::Create(device_, physicalDevice_->queueFamily(), error);
    if (frames_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return false;
    }

    profiler_ = GpuProfiler::Create(*this, error);
    if (profiler_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return false;
    }
    return true;
}

void Device::OnSurfaceChanged(int32_t width, int32_t height) {
    if (!HasSurface()) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "surfaceChanged with no surface held");
        return;
    }

    if (width == width_ && height == height_) {
        // Format-only change. Nothing the swapchain cares about: Vulkan picks
        // its own format through vkGetPhysicalDeviceSurfaceFormatsKHR and
        // ignores the PixelFormat the SurfaceHolder reports.
        return;
    }

    width_ = width;
    height_ = height;
    swapchainOutOfDate_ = true;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "surfaceChanged: %dx%d", width_, height_);
}

bool Device::RecreateSwapchainIfNeeded() {
    if (!swapchainOutOfDate_ && swapchain_ != nullptr) {
        return true;
    }
    if (!HasSurface()) {
        return false;
    }

    // Everything that might still be reading the old images has to be done
    // before they can be handed back. Blunt but correct; a per frame fence wait
    // would let the recreation overlap the tail of the previous frame, and that
    // is an optimisation for when there are frames worth overlapping.
    vkDeviceWaitIdle(device_);

    std::string error;
    const VkSwapchainKHR oldHandle =
            swapchain_ != nullptr ? swapchain_->handle() : VK_NULL_HANDLE;
    std::unique_ptr<Swapchain> fresh = Swapchain::Create(physicalDevice_->handle(), device_,
                                                        surface_, oldHandle, error);
    if (fresh == nullptr) {
        // A window with no area lands here while a resize is in flight. Not
        // fatal: stay out of date and try again on the next frame.
        __android_log_print(ANDROID_LOG_WARN, kLogTag, "swapchain not created: %s", error.c_str());
        return false;
    }

    // Assigning destroys the old one, which is why oldHandle had to be read
    // first: vkCreateSwapchainKHR needs it alive to hand its buffers over, and
    // it may only be destroyed afterwards.
    swapchain_ = std::move(fresh);
    swapchainOutOfDate_ = false;
    __android_log_print(ANDROID_LOG_INFO, kLogTag, "swapchain: %s",
                        swapchain_->Describe().c_str());
    return true;
}

void Device::OnSurfaceDestroyed() {
    if (window_ == nullptr) {
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "surfaceDestroyed: tearing down");

    // Order matters and it is strictly inside out. Nothing may still reference
    // the window when this returns — the UI thread is blocked on it and Android
    // invalidates the Surface the moment it does.
    if (device_ != VK_NULL_HANDLE) {
        // Blocks until every submission has retired. Destroying a device with
        // work in flight is undefined behaviour, and on Android the usual
        // symptom is a driver crash inside the compositor rather than anything
        // that points back here.
        vkDeviceWaitIdle(device_);
        // Before the device: everything below was created from it.
        //
        // The world goes first of all. Its meshes are allocations from the
        // allocator three lines down, and its render targets are images from the
        // same place — outliving either would be a use-after-free that
        // vmaDestroyAllocator would only report as a leak assert.
        world_.reset();
        swapchain_.reset();
        profiler_.reset();
        frames_.reset();
        // After the world: its materials and passes hold sets allocated here,
        // and destroying a pool frees every set that came out of it.
        descriptorPool_.reset();
        // Last of the three, because buffers and images are carved out of it.
        // vmaDestroyAllocator asserts in debug when an allocation is still
        // alive, which is exactly the signal wanted if a mesh ever outlives the
        // surface it was loaded for.
        allocator_.reset();
        vkDestroyDevice(device_, nullptr);
        device_ = VK_NULL_HANDLE;
        // volk's global device-level pointers now refer to a destroyed device.
        // Nothing may call one until the next volkLoadDevice, which is
        // guaranteed by there being no frame loop without a surface.
    }
    queue_ = VK_NULL_HANDLE;
    physicalDevice_.reset();

    if (surface_ != VK_NULL_HANDLE) {
        // Same instance that created it, kept in this object precisely so this
        // line cannot pick up the wrong one.
        vkDestroySurfaceKHR(instance_, surface_, nullptr);
        surface_ = VK_NULL_HANDLE;
    }
    instance_ = VK_NULL_HANDLE;

    // Last, because the surface was consuming this very reference.
    ANativeWindow_release(window_);
    window_ = nullptr;
    width_ = 0;
    height_ = 0;
    swapchainOutOfDate_ = false;
}

namespace device_holder {

Device& Get() {
    // Function-local static: constructed on first use, thread-safe since C++11,
    // and there is exactly one for the life of the loaded library — same
    // lifetime story as the instance, except this one is routinely empty.
    static Device device;
    return device;
}

} // namespace device_holder

} // namespace putorana::graphics
