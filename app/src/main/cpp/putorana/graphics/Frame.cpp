#include "Frame.h"

#include "Device.h"
#include "World.h"
#include "putorana/ar/Subsystem.h"
#include "putorana/worlds/WorldRegistry.h"

#include <android/log.h>

#include <algorithm>
#include <cmath>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";
constexpr int64_t kOneSecondNanos = 1000000000;

// Frame loop heartbeat. It only exists so the loop is visible in logcat while
// DrawFrame has nothing to show; delete it once there are pixels to look at.
// Render thread only, like everything else in this file, so plain globals are
// enough.
int64_t g_reportStartNanos = 0;
uint32_t g_framesSinceReport = 0;

/** Vsync timestamp of the previous frame, for the delta handed to the world. */
int64_t g_lastFrameNanos = 0;

/**
 * Seconds since the previous frame, clamped.
 *
 * The clamp is the whole reason this is a function. Coming back from background
 * produces a gap of seconds, and every consumer of a delta — animation, physics,
 * a follow camera — integrates it: unclamped, the first frame back teleports
 * everything. Capping at 100ms turns that into a hitch instead, which is what
 * every engine does and for the same reason.
 * */
float DeltaSeconds(int64_t frameTimeNanos) {
    constexpr int64_t kMaxDeltaNanos = 100000000; // 100ms

    if (g_lastFrameNanos == 0) {
        g_lastFrameNanos = frameTimeNanos;
        return 0.0f;
    }
    const int64_t delta =
            std::clamp(frameTimeNanos - g_lastFrameNanos, int64_t{0}, kMaxDeltaNanos);
    g_lastFrameNanos = frameTimeNanos;
    return static_cast<float>(delta) / 1e9f;
}

void Heartbeat(int64_t frameTimeNanos) {
    ++g_framesSinceReport;

    const int64_t elapsed = frameTimeNanos - g_reportStartNanos;
    if (g_reportStartNanos == 0 || elapsed > 2 * kOneSecondNanos) {
        // First frame, or a gap big enough that the app was clearly in the
        // background. Restart the window instead of reporting a fake rate.
        g_reportStartNanos = frameTimeNanos;
        g_framesSinceReport = 0;
        return;
    }

    if (elapsed < kOneSecondNanos) {
        return;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "frame loop: %u frames in %.2fs (%.1f fps)",
                        g_framesSinceReport, static_cast<double>(elapsed) / 1e9,
                        static_cast<double>(g_framesSinceReport) * 1e9 /
                                static_cast<double>(elapsed));
    g_reportStartNanos = frameTimeNanos;
    g_framesSinceReport = 0;
}

/**
 * A slow sweep through the hues. Placeholder content, but a moving one: a static
 * colour cannot be told apart from a frame loop that died three seconds ago.
 * Driven off the vsync timestamp rather than a frame counter so it stays at the
 * same speed whatever the refresh rate is.
 * */
VkClearColorValue ClearColor(int64_t frameTimeNanos) {
    const double seconds = static_cast<double>(frameTimeNanos) / 1e9;
    const auto channel = [seconds](double offset) {
        return static_cast<float>(0.5 + 0.5 * std::sin(seconds * 0.6 + offset));
    };

    VkClearColorValue color{};
    color.float32[0] = channel(0.0);
    color.float32[1] = channel(2.0944); // 2pi/3
    color.float32[2] = channel(4.1888); // 4pi/3
    color.float32[3] = 1.0f;
    return color;
}

VkImageMemoryBarrier2 ColorBarrier(VkImage image, VkImageLayout oldLayout,
                                   VkImageLayout newLayout) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;
    return barrier;
}

void SubmitBarrier(VkCommandBuffer commandBuffer, const VkImageMemoryBarrier2& barrier) {
    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

/**
 * The placeholder content, for when no world has been installed. Kept because a
 * static colour cannot be told apart from a frame loop that died three seconds
 * ago, and because it proves acquire/submit/present work before there is
 * anything to draw.
 * */
void RecordEmptyClear(VkCommandBuffer commandBuffer, const Swapchain& swapchain,
                      uint32_t imageIndex, int64_t frameTimeNanos) {
    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = swapchain.imageView(imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    colorAttachment.clearValue.color = ClearColor(frameTimeNanos);

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = swapchain.extent();
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAttachment;

    // Dynamic rendering: no VkRenderPass, no VkFramebuffer, no subpass. The
    // clear is the loadOp, so an empty pair of calls is a full screen clear.
    vkCmdBeginRendering(commandBuffer, &rendering);
    vkCmdEndRendering(commandBuffer);
}

/**
 * Wraps whatever draws in the two things the frame loop owns and a world must
 * not: the command buffer's lifetime, and the layout the presented image has to
 * be in on the way in and on the way out.
 *
 * The split is deliberate. Acquiring, transitioning and presenting are the same
 * for every world that will ever exist; which passes run, in what order, into
 * what targets, is different for every one of them. So this function knows
 * nothing about passes and World::Render knows nothing about the swapchain's
 * layout.
 * */
void RecordFrame(VkCommandBuffer commandBuffer, const Swapchain& swapchain, uint32_t imageIndex,
                 uint32_t frameIndex, World* world, int64_t frameTimeNanos) {
    VkCommandBufferBeginInfo beginInfo{};
    beginInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
    // The pool was created with RESET_COMMAND_BUFFER_BIT, so this resets the
    // buffer on its own. ONE_TIME_SUBMIT lets the driver skip anything it would
    // need to keep for a second submission.
    beginInfo.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(commandBuffer, &beginInfo);

    // First thing in the buffer, and it has to be: this records the query pool
    // reset, which must precede every timestamp written into it this frame.
    GpuProfiler& profiler = device_holder::Get().profiler();
    profiler.BeginFrame(commandBuffer, frameIndex);

    // UNDEFINED as the old layout throws the previous contents away, and on a
    // tiler that is not a small thing: preserving pixels means reading the whole
    // image back into tile memory before drawing over it.
    //
    // It is only correct because whatever runs below covers every pixel — the
    // placeholder is a full screen clear, and a world's final pass is a full
    // screen quad. A world that ever wants to composite ONTO the previous
    // frame's image would need this to become PRESENT_SRC_KHR, and would need
    // the swapchain's image count to be part of its reasoning.
    VkImageMemoryBarrier2 toAttachment =
            ColorBarrier(swapchain.image(imageIndex), VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    // Both sides sit at COLOR_ATTACHMENT_OUTPUT, matching the stage the acquire
    // semaphore is waited on, so the transition cannot run ahead of it.
    toAttachment.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.srcAccessMask = 0;
    toAttachment.dstStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toAttachment.dstAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    SubmitBarrier(commandBuffer, toAttachment);

    if (world != nullptr) {
        FrameContext frame;
        frame.commandBuffer = commandBuffer;
        frame.frameIndex = frameIndex;
        frame.swapchain = &swapchain;
        frame.imageIndex = imageIndex;
        frame.profiler = &profiler;
        // The outermost scope, so the overlay can show what the passes inside it
        // do NOT account for — barriers, transitions, and whatever the driver
        // does between them.
        GpuScope scope(frame, "frame");
        world->Render(frame);
    } else {
        RecordEmptyClear(commandBuffer, swapchain, imageIndex, frameTimeNanos);
    }

    VkImageMemoryBarrier2 toPresent =
            ColorBarrier(swapchain.image(imageIndex), VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    toPresent.srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;
    toPresent.srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT;
    // Nothing in this submission follows, and the presentation engine is
    // synchronised by the semaphore rather than by this barrier. NONE says that
    // precisely; BOTTOM_OF_PIPE would be the old way of writing the same no-op.
    toPresent.dstStageMask = VK_PIPELINE_STAGE_2_NONE;
    toPresent.dstAccessMask = 0;
    SubmitBarrier(commandBuffer, toPresent);

    vkEndCommandBuffer(commandBuffer);
}

} // namespace

void DrawFrame(int64_t frameTimeNanos) {
    Device& device = device_holder::Get();

    // The loop is stopped before the surface is torn down, but Choreographer can
    // still deliver one callback that was already queued, so this is a real case
    // rather than paranoia.
    if (!device.HasSurface()) {
        return;
    }

    // Resize path #1: something changed the window, so the swapchain has to be
    // rebuilt before anything can be acquired from it. Returns false when the
    // window has no area yet, in which case there is nothing to draw into.
    if (!device.RecreateSwapchainIfNeeded()) {
        return;
    }

    Heartbeat(frameTimeNanos);

    // Builds the first world, and every world the menu switches to afterwards.
    // Here and not in Device::OnSurfaceCreated because a world's pipelines are
    // built for the surface's colour format, which is only known once the
    // swapchain exists — and RecreateSwapchainIfNeeded has just guaranteed one.
    //
    // This namespace no longer names a concrete world at all: which scene is
    // showing is putorana::worlds' business, and everything here works through
    // the World interface.
    worlds::InstallIfNeeded(device);

    // The AR session's tick, and it is FIRST for two reasons.
    //
    // It blocks — up to ARCore's built-in 66ms — waiting for the next camera
    // image, so it has to happen before BeginFrame and before the acquire below,
    // or it would sleep while holding a swapchain image and a frame slot.
    //
    // And it is the thing everything else this frame is about: the camera image
    // the background draws, and soon the pose the world's camera follows. A tick
    // buried inside a render pass would leave whichever pass ran first working
    // from the previous frame's answer, which in AR is visible as geometry
    // sliding against the world.
    if (ar::Subsystem* arSubsystem = ar::subsystem_holder::Get()) {
        arSubsystem->Update();
    }

    // Simulation before recording, and outside the acquire: the world touches no
    // Vulkan object here, it only moves nodes and closes their matrices. Nothing
    // it does needs to wait on an image.
    World* world = device.world();
    if (world != nullptr) {
        world->Update(DeltaSeconds(frameTimeNanos));
    } else {
        // Keep the clock advancing anyway, so the first delta after a world is
        // installed is one frame and not the whole time the app has been up.
        (void)DeltaSeconds(frameTimeNanos);
    }

    const Swapchain& swapchain = device.swapchain();
    FrameRing& frames = device.frames();

    // Blocks until the slot about to be reused has retired on the GPU. This is
    // the only thing stopping the CPU from running arbitrarily far ahead.
    const FrameSlot slot = frames.BeginFrame();

    uint32_t imageIndex = 0;
    const VkResult acquired =
            vkAcquireNextImageKHR(device.handle(), swapchain.handle(), UINT64_MAX,
                                  slot.imageAvailable, VK_NULL_HANDLE, &imageIndex);

    // Resize path #2, and on Android the one that actually fires on rotation:
    // the compositor can change the surface transform without the window size
    // changing at all, so waiting for surfaceChanged would mean presenting
    // rotated garbage.
    if (acquired == VK_ERROR_OUT_OF_DATE_KHR) {
        // Nothing was acquired, so the semaphore was not signalled and nothing
        // is left dangling. Bail and rebuild next frame. The ring is not
        // advanced: no submission means no timeline signal to wait for.
        device.InvalidateSwapchain();
        return;
    }
    if (acquired != VK_SUCCESS && acquired != VK_SUBOPTIMAL_KHR) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "vkAcquireNextImageKHR failed: %d",
                            acquired);
        return;
    }
    if (acquired == VK_SUBOPTIMAL_KHR) {
        // Different from OUT_OF_DATE in the one way that matters: the semaphore
        // *was* signalled. Abandoning the frame here would leave it signalled
        // with nobody to wait on it, so the frame is finished normally and the
        // swapchain is rebuilt on the next one.
        device.InvalidateSwapchain();
    }

    RecordFrame(slot.commandBuffer, swapchain, imageIndex, slot.index, world, frameTimeNanos);

    VkCommandBufferSubmitInfo commandBufferInfo{};
    commandBufferInfo.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO;
    commandBufferInfo.commandBuffer = slot.commandBuffer;

    VkSemaphoreSubmitInfo waitInfo{};
    waitInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    waitInfo.semaphore = slot.imageAvailable;
    // Only the colour writes have to wait for the image. Everything before that
    // stage may start immediately, which is the whole point of a per semaphore
    // stage mask.
    waitInfo.stageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT;

    // Binary and timeline in one array, which is the ergonomic win of
    // synchronization2: before it, timeline values lived in a parallel
    // VkTimelineSemaphoreSubmitInfo whose arrays had to be kept index-aligned by
    // hand. The value field is simply ignored for the binary one.
    VkSemaphoreSubmitInfo signalInfos[2]{};
    signalInfos[0].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[0].semaphore = swapchain.renderFinishedSemaphore(imageIndex);
    signalInfos[0].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;
    signalInfos[1].sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO;
    signalInfos[1].semaphore = frames.timeline();
    signalInfos[1].value = slot.signalValue;
    signalInfos[1].stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT;

    VkSubmitInfo2 submit{};
    submit.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2;
    submit.waitSemaphoreInfoCount = 1;
    submit.pWaitSemaphoreInfos = &waitInfo;
    submit.commandBufferInfoCount = 1;
    submit.pCommandBufferInfos = &commandBufferInfo;
    submit.signalSemaphoreInfoCount = 2;
    submit.pSignalSemaphoreInfos = signalInfos;

    const VkResult submitted = vkQueueSubmit2(device.queue(), 1, &submit, VK_NULL_HANDLE);
    if (submitted != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "vkQueueSubmit2 failed: %d", submitted);
        // Deliberately not advancing the ring: the timeline was never signalled,
        // and BeginFrame would then wait for a value that can never arrive.
        return;
    }

    VkSemaphore presentWait = swapchain.renderFinishedSemaphore(imageIndex);
    VkSwapchainKHR swapchainHandle = swapchain.handle();
    VkPresentInfoKHR presentInfo{};
    presentInfo.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
    presentInfo.waitSemaphoreCount = 1;
    presentInfo.pWaitSemaphores = &presentWait;
    presentInfo.swapchainCount = 1;
    presentInfo.pSwapchains = &swapchainHandle;
    presentInfo.pImageIndices = &imageIndex;

    const VkResult presented = vkQueuePresentKHR(device.queue(), &presentInfo);
    if (presented == VK_ERROR_OUT_OF_DATE_KHR || presented == VK_SUBOPTIMAL_KHR) {
        device.InvalidateSwapchain();
    } else if (presented != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "vkQueuePresentKHR failed: %d", presented);
    }

    // The submission went through, so the timeline will be signalled whatever
    // present did. Safe to move on.
    frames.EndFrame();
}

void ResetFrameStats() {
    g_reportStartNanos = 0;
    g_framesSinceReport = 0;
    // Also the delta clock. Without this the first frame of the next surface
    // measures across however long the app spent in background, and the clamp
    // in DeltaSeconds would be doing the work a reset should have done.
    g_lastFrameNanos = 0;
    // The world went with the device. The registry keeps WHICH world was being
    // shown and only forgets that it built it, so the next surface rebuilds the
    // same one.
    worlds::OnSurfaceLost();
}

} // namespace putorana::graphics
