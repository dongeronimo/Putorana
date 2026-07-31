#ifndef PUTORANA_GRAPHICS_GPUPROFILER_H
#define PUTORANA_GRAPHICS_GPUPROFILER_H

#include "FrameRing.h"
#include "volk.h"

#include <array>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace putorana::graphics {

class Device;
struct FrameContext;

/** How long one named scope took ON THE GPU, in milliseconds. */
struct PassTiming {
    std::string name;
    float milliseconds = 0.0f;
};

/**
 * GPU timing for named scopes, by timestamp query.
 *
 * ## Why this cannot just be a clock read around the calls
 *
 * Recording a command buffer takes almost no time and tells you nothing: the
 * work has not happened yet. vkCmdDraw returns long before the GPU has drawn
 * anything, so a CPU timer around a render pass measures how fast the driver
 * builds a command list. The only way to learn what the GPU spent is to have the
 * GPU itself write the time, which is what a timestamp query is — a value the
 * device records when everything before it in the pipeline has finished.
 *
 * ## Why the results are always a frame or two old
 *
 * A timestamp is readable only once the submission carrying it has retired. This
 * keeps one query pool per frame in flight and reads a slot's results at the
 * start of the NEXT frame that reuses it — by which point FrameRing::BeginFrame
 * has already waited on the timeline for exactly that, so the read never blocks.
 * The alternative, waiting for the frame that just ended, would stall the CPU on
 * the GPU every frame to measure the GPU, which is a fine way to make the numbers
 * wrong in the direction of "everything is fast".
 *
 * So what Snapshot returns is kFramesInFlight frames behind. For a debug overlay
 * that is invisible.
 *
 * ## Scopes nest
 *
 * Begin/End pairs form a stack, so a scope around the whole frame can contain one
 * per pass. Timestamps are written with ALL_COMMANDS on both ends: the opening
 * one lands when everything recorded before it has completed, the closing one
 * when the scope's own work has. That is the conservative reading, and it means
 * two adjacent scopes cannot appear to overlap.
 * */
class GpuProfiler {
public:
    /** Plenty: this is a debug facility, not a frame graph. */
    static constexpr uint32_t kMaxScopes = 16;

    /**
     * Null with the reason in `error` only if the pools cannot be made. A device
     * whose queue family reports no timestamp support is NOT a failure — the
     * profiler is created disabled and every call becomes a no-op, because a
     * renderer that refuses to start on such a device would be trading a working
     * app for a debug overlay.
     * */
    static std::unique_ptr<GpuProfiler> Create(Device& device, std::string& error);

    ~GpuProfiler();

    GpuProfiler(const GpuProfiler&) = delete;
    GpuProfiler& operator=(const GpuProfiler&) = delete;

    /**
     * Reads back this slot's previous results and resets its pool for reuse.
     *
     * Must be recorded FIRST, before anything else goes into the command buffer:
     * vkCmdResetQueryPool has to precede every write to the queries it clears.
     * */
    void BeginFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex);

    void BeginScope(VkCommandBuffer commandBuffer, const char* name);
    void EndScope(VkCommandBuffer commandBuffer);

    /**
     * The most recently completed frame's timings, exponentially smoothed.
     *
     * Safe from any thread — it is read from the UI thread to build the overlay
     * while the render thread is writing the next frame's numbers. Raw per-frame
     * GPU times jump around by tens of percent frame to frame, so the smoothing
     * is not cosmetic: unsmoothed, the overlay is unreadable.
     * */
    std::vector<PassTiming> Snapshot() const;

    bool enabled() const { return enabled_; }

private:
    GpuProfiler() = default;

    void ReadBack(uint32_t frameIndex);

    VkDevice handle_ = VK_NULL_HANDLE;
    bool enabled_ = false;

    /** Nanoseconds per tick, from VkPhysicalDeviceLimits. */
    float timestampPeriod_ = 1.0f;
    /** Ticks above this many bits are garbage and must be masked off. */
    uint64_t timestampMask_ = ~uint64_t{0};

    std::array<VkQueryPool, FrameRing::kFramesInFlight> pools_{};

    /** Names recorded into each slot, in the order their queries were written. */
    std::array<std::vector<std::string>, FrameRing::kFramesInFlight> names_{};
    /** True once a slot has been recorded into, so the first pass reads nothing. */
    std::array<bool, FrameRing::kFramesInFlight> recorded_{};

    uint32_t currentFrame_ = 0;
    /** Open scopes, innermost last. Holds query pair indices. */
    std::vector<uint32_t> openScopes_;

    mutable std::mutex mutex_;
    std::vector<PassTiming> smoothed_;
};

/**
 * Opens a GPU scope for as long as it lives.
 *
 * A scope has to be closed on every path out, and the paths out of a render
 * function are early returns. This is the shape that survives adding one.
 * */
class GpuScope {
public:
    GpuScope(const FrameContext& frame, const char* name);
    ~GpuScope();

    GpuScope(const GpuScope&) = delete;
    GpuScope& operator=(const GpuScope&) = delete;

private:
    GpuProfiler* profiler_ = nullptr;
    VkCommandBuffer commandBuffer_ = VK_NULL_HANDLE;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_GPUPROFILER_H
