#include "DescriptorPool.h"

#include <android/log.h>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * One block's budget. The ratios are a guess at the shape of a frame's worth of
 * descriptors — a couple of uniform buffers and storage buffers per set, more
 * combined samplers because textured materials are what multiply. Being wrong
 * costs a second block, which is the entire point of the design.
 * */
constexpr uint32_t kSetsPerBlock = 64;

constexpr VkDescriptorPoolSize kBlockSizes[] = {
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * kSetsPerBlock},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2 * kSetsPerBlock},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 4 * kSetsPerBlock},
};

} // namespace

std::unique_ptr<DescriptorPool> DescriptorPool::Create(VkDevice device, std::string& error) {
    auto pool = std::unique_ptr<DescriptorPool>(new DescriptorPool());
    pool->device_ = device;
    if (!pool->AddBlock()) {
        error = "could not create the first descriptor pool block";
        return nullptr;
    }
    return pool;
}

DescriptorPool::~DescriptorPool() {
    // Destroying a pool frees every set allocated from it, which is why nothing
    // tracks the sets individually.
    for (VkDescriptorPool block : blocks_) {
        vkDestroyDescriptorPool(device_, block, nullptr);
    }
}

bool DescriptorPool::AddBlock() {
    VkDescriptorPoolCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    // No FREE_DESCRIPTOR_SET_BIT on purpose — see the class comment.
    info.maxSets = kSetsPerBlock;
    info.poolSizeCount = static_cast<uint32_t>(std::size(kBlockSizes));
    info.pPoolSizes = kBlockSizes;

    VkDescriptorPool block = VK_NULL_HANDLE;
    const VkResult result = vkCreateDescriptorPool(device_, &info, nullptr, &block);
    if (result != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "vkCreateDescriptorPool failed: %d",
                            result);
        return false;
    }
    blocks_.push_back(block);
    return true;
}

VkDescriptorSet DescriptorPool::Allocate(VkDescriptorSetLayout layout) {
    for (int attempt = 0; attempt < 2; ++attempt) {
        VkDescriptorSetAllocateInfo info{};
        info.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        info.descriptorPool = blocks_.back();
        info.descriptorSetCount = 1;
        info.pSetLayouts = &layout;

        VkDescriptorSet set = VK_NULL_HANDLE;
        const VkResult result = vkAllocateDescriptorSets(device_, &info, &set);
        if (result == VK_SUCCESS) {
            return set;
        }
        // These two, and only these two, mean "this block is used up". Every
        // other result is a real out-of-memory and retrying would just fail
        // again in a fresh block.
        if (result != VK_ERROR_OUT_OF_POOL_MEMORY && result != VK_ERROR_FRAGMENTED_POOL) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "vkAllocateDescriptorSets failed: %d",
                                result);
            return VK_NULL_HANDLE;
        }
        if (!AddBlock()) {
            return VK_NULL_HANDLE;
        }
    }
    // A brand new block refusing the very first set means the layout asks for
    // more of some descriptor type than a whole block holds. That is a design
    // problem in the layout, not something to retry.
    __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                        "descriptor set does not fit in a fresh pool block; the layout needs more "
                        "descriptors of some type than a block budgets for");
    return VK_NULL_HANDLE;
}

} // namespace putorana::graphics
