#ifndef PUTORANA_GRAPHICS_DESCRIPTORPOOL_H
#define PUTORANA_GRAPHICS_DESCRIPTORPOOL_H

#include "volk.h"

#include <memory>
#include <string>
#include <vector>

namespace putorana::graphics {

/**
 * Where every VkDescriptorSet in the renderer comes from.
 *
 * A VkDescriptorPool has a fixed budget, declared up front, of how many sets and
 * how many descriptors of each type it can hand out. Guess low and allocation
 * starts failing halfway through loading a scene; guess high and the memory is
 * reserved whether or not anything uses it. Neither is a number worth trying to
 * get right, so this grows instead: when a pool refuses, another is created and
 * the allocation is retried. The budget below is only the size of one block.
 *
 * Sets are never freed individually. Descriptor sets in this renderer live as
 * long as the thing that owns them (a material, a render pass) and the whole
 * chain of pools is destroyed at once with the Device. That is also why the
 * pools are not created with FREE_DESCRIPTOR_SET_BIT: not asking for it lets
 * the driver use a simple bump allocator inside each block.
 *
 * SWITCHING WORLDS LEAKS INTO THIS, and the leak is by construction rather than
 * by accident. A world destroys its materials and its passes, and their sets
 * stay allocated until the Device goes, so every switch costs about nine sets
 * that nothing will ever hand back: two per MeshPass frame resource, one for the
 * final pass, one per material. At 64 sets to a block that is a new block every
 * seven or eight switches, which is slow enough to be invisible in an app
 * session and unbounded all the same.
 *
 * The fix, when it is worth making, is a pool PER WORLD rather than per Device:
 * destroying it reclaims every set in one call, and it restores the "everything
 * dies together" premise this class was designed around instead of working
 * against it. Freeing sets individually is the wrong half of the trade: it
 * would cost the bump allocator on every allocation to reclaim memory that a
 * scoped pool gives back for free.
 * */
class DescriptorPool {
public:
    static std::unique_ptr<DescriptorPool> Create(VkDevice device, std::string& error);

    ~DescriptorPool();

    DescriptorPool(const DescriptorPool&) = delete;
    DescriptorPool& operator=(const DescriptorPool&) = delete;

    /**
     * A set with the given layout, or VK_NULL_HANDLE with the reason logged.
     *
     * The only two failures that mean "this pool is full", OUT_OF_POOL_MEMORY
     * and FRAGMENTED_POOL, are handled here by starting a new block. Anything
     * else is out of memory for real and is reported.
     * */
    VkDescriptorSet Allocate(VkDescriptorSetLayout layout);

private:
    DescriptorPool() = default;

    /** Appends a fresh block and makes it current. False with the reason logged. */
    bool AddBlock();

    VkDevice device_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorPool> blocks_;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_DESCRIPTORPOOL_H
