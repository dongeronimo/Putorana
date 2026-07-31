#include "GpuProfiler.h"

#include "Device.h"
#include "World.h"

#include <android/log.h>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * How fast a number chases the truth, per frame. 0.1 settles in a few tenths of
 * a second at 30fps, which is slow enough to read and fast enough to notice a
 * change while you are still looking at the thing that caused it.
 * */
constexpr float kSmoothing = 0.1f;

} // namespace

std::unique_ptr<GpuProfiler> GpuProfiler::Create(Device& device, std::string& error) {
    auto profiler = std::unique_ptr<GpuProfiler>(new GpuProfiler());
    profiler->handle_ = device.handle();

    const VkPhysicalDeviceLimits& limits = device.physicalDevice().properties().limits;
    profiler->timestampPeriod_ = limits.timestampPeriod;

    // timestampPeriod of zero means the device does not support timestamps at
    // all, and every duration would come out as zero rather than as an error.
    if (limits.timestampPeriod <= 0.0f) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "GPU profiler off: the device reports no timestamp period");
        return profiler;
    }

    // Support is PER QUEUE FAMILY, not per device: a family may report zero
    // valid bits even where the limit above looks fine. The bits also say how
    // much of each 64-bit result is real — the rest is undefined and has to be
    // masked off, or a duration comes out as a plausible-looking nonsense.
    uint32_t familyCount = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice().handle(), &familyCount,
                                             nullptr);
    std::vector<VkQueueFamilyProperties> families(familyCount);
    vkGetPhysicalDeviceQueueFamilyProperties(device.physicalDevice().handle(), &familyCount,
                                             families.data());

    const uint32_t family = device.physicalDevice().queueFamily();
    const uint32_t validBits = family < familyCount ? families[family].timestampValidBits : 0;
    if (validBits == 0) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "GPU profiler off: queue family %u supports no timestamps", family);
        return profiler;
    }
    // Shifting by 64 is undefined, and 64 valid bits is the common case.
    profiler->timestampMask_ =
            validBits >= 64 ? ~uint64_t{0} : (uint64_t{1} << validBits) - uint64_t{1};

    VkQueryPoolCreateInfo poolInfo{};
    poolInfo.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO;
    poolInfo.queryType = VK_QUERY_TYPE_TIMESTAMP;
    poolInfo.queryCount = kMaxScopes * 2;
    for (uint32_t i = 0; i < FrameRing::kFramesInFlight; ++i) {
        if (vkCreateQueryPool(profiler->handle_, &poolInfo, nullptr, &profiler->pools_[i]) !=
            VK_SUCCESS) {
            error = "GPU profiler: could not create a query pool";
            return nullptr;
        }
    }

    profiler->enabled_ = true;
    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "GPU profiler on: %.2f ns/tick, %u valid bits", limits.timestampPeriod,
                        validBits);
    return profiler;
}

GpuProfiler::~GpuProfiler() {
    for (VkQueryPool pool : pools_) {
        if (pool != VK_NULL_HANDLE) {
            vkDestroyQueryPool(handle_, pool, nullptr);
        }
    }
}

void GpuProfiler::BeginFrame(VkCommandBuffer commandBuffer, uint32_t frameIndex) {
    if (!enabled_) {
        return;
    }
    currentFrame_ = frameIndex;
    openScopes_.clear();

    // This slot's previous submission has retired — FrameRing::BeginFrame waited
    // on the timeline for it — so its results are sitting there readable. Read
    // before the reset below throws them away.
    if (recorded_[frameIndex]) {
        ReadBack(frameIndex);
    }

    vkCmdResetQueryPool(commandBuffer, pools_[frameIndex], 0, kMaxScopes * 2);
    names_[frameIndex].clear();
    recorded_[frameIndex] = false;
}

void GpuProfiler::BeginScope(VkCommandBuffer commandBuffer, const char* name) {
    if (!enabled_) {
        return;
    }
    const uint32_t scope = static_cast<uint32_t>(names_[currentFrame_].size());
    if (scope >= kMaxScopes) {
        return;
    }
    names_[currentFrame_].emplace_back(name);
    openScopes_.push_back(scope);
    recorded_[currentFrame_] = true;

    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         pools_[currentFrame_], scope * 2);
}

void GpuProfiler::EndScope(VkCommandBuffer commandBuffer) {
    if (!enabled_ || openScopes_.empty()) {
        return;
    }
    const uint32_t scope = openScopes_.back();
    openScopes_.pop_back();

    vkCmdWriteTimestamp2(commandBuffer, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                         pools_[currentFrame_], scope * 2 + 1);
}

void GpuProfiler::ReadBack(uint32_t frameIndex) {
    const std::vector<std::string>& names = names_[frameIndex];
    if (names.empty()) {
        return;
    }
    const uint32_t queryCount = static_cast<uint32_t>(names.size()) * 2;

    std::vector<uint64_t> raw(queryCount, 0);
    // No WAIT_BIT: the submission has retired, so the results are there. Asking
    // to wait would be asking the CPU to block on the GPU in order to measure
    // it. VK_NOT_READY is still possible if a frame was abandoned between the
    // reset and the submit, and it simply means there is nothing to report.
    const VkResult read = vkGetQueryPoolResults(
            handle_, pools_[frameIndex], 0, queryCount, raw.size() * sizeof(uint64_t), raw.data(),
            sizeof(uint64_t), VK_QUERY_RESULT_64_BIT);
    if (read != VK_SUCCESS) {
        return;
    }

    std::lock_guard<std::mutex> lock(mutex_);
    for (size_t i = 0; i < names.size(); ++i) {
        const uint64_t begin = raw[i * 2] & timestampMask_;
        const uint64_t end = raw[i * 2 + 1] & timestampMask_;
        // Unsigned subtraction the wrong way round would wrap to something
        // enormous rather than going negative, so it is checked rather than
        // clamped afterwards.
        const float milliseconds =
                end > begin ? static_cast<float>(end - begin) * timestampPeriod_ * 1e-6f : 0.0f;

        if (i >= smoothed_.size()) {
            smoothed_.push_back(PassTiming{names[i], milliseconds});
            continue;
        }
        if (smoothed_[i].name != names[i]) {
            // The set of scopes changed — a world swapped, say. Restart rather
            // than blending one pass's numbers into another's.
            smoothed_[i] = PassTiming{names[i], milliseconds};
            continue;
        }
        smoothed_[i].milliseconds += kSmoothing * (milliseconds - smoothed_[i].milliseconds);
    }
    smoothed_.resize(names.size());
}

std::vector<PassTiming> GpuProfiler::Snapshot() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return smoothed_;
}

GpuScope::GpuScope(const FrameContext& frame, const char* name)
        : profiler_(frame.profiler), commandBuffer_(frame.commandBuffer) {
    if (profiler_ != nullptr) {
        profiler_->BeginScope(commandBuffer_, name);
    }
}

GpuScope::~GpuScope() {
    if (profiler_ != nullptr) {
        profiler_->EndScope(commandBuffer_);
    }
}

} // namespace putorana::graphics
