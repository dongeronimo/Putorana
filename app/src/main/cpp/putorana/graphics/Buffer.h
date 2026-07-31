#ifndef PUTORANA_GRAPHICS_BUFFER_H
#define PUTORANA_GRAPHICS_BUFFER_H

#include "volk.h"
#include "vk_mem_alloc.h"

#include <memory>
#include <string>

namespace putorana::graphics {

/**
 * A VkBuffer plus the VMA allocation behind it, permanently mapped and written
 * straight from the CPU. Vertex data, index data, uniforms, model matrices — all
 * of it goes through here.
 *
 * No staging buffer anywhere in this class, and that is a deliberate consequence
 * of the platform rather than a shortcut. A discrete GPU has memory the CPU
 * cannot reach, so uploading means writing a HOST_VISIBLE buffer and issuing a
 * vkCmdCopyBuffer into a DEVICE_LOCAL one. An Android GPU has no such split:
 * there is one pool of RAM, and DEVICE_LOCAL|HOST_VISIBLE memory types exist
 * (Allocator::unifiedMemory() checks). Copying would be memcpy'ing RAM to RAM.
 *
 * The catch is what that memory usually *is*: write-combined and uncached. Writes
 * are fast and reads are catastrophically slow — tens of times slower, because
 * every read goes to DRAM. So the rules for the mapped pointer are:
 *
 *   - write forward, in order, ideally with memcpy;
 *   - never read from it, not even for a read-modify-write;
 *   - never use it as scratch space.
 *
 * VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT states exactly that
 * contract to VMA, and it is what lets it hand back the fast memory type.
 *
 * Device lifetime: the Allocator dies with the VkDevice, so every Buffer has to
 * be gone before the surface is torn down.
 * */
class Buffer {
public:
    /**
     * `usage` carries no TRANSFER_DST — nothing copies into these. `name` shows
     * up in VMA's stats dump and is kept alive by this object.
     * */
    static std::unique_ptr<Buffer> Create(VmaAllocator allocator, VkDeviceSize size,
                                          VkBufferUsageFlags usage, std::string name,
                                          std::string& error);

    ~Buffer();

    Buffer(const Buffer&) = delete;
    Buffer& operator=(const Buffer&) = delete;

    VkBuffer handle() const { return handle_; }
    VkDeviceSize size() const { return size_; }

    /**
     * memcpy into the mapping, then flush. Bounds-checked: a write that would
     * run past the end is refused and logged rather than corrupting whatever
     * VMA suballocated next door.
     * */
    bool Write(const void* data, VkDeviceSize bytes, VkDeviceSize offset = 0);

    /**
     * The raw mapping, for callers that build their data in place instead of
     * copying it from somewhere else (index conversion does this). Whoever uses
     * this owes a Flush over the range they touched, and owes it before the
     * command buffer that reads the range is submitted.
     *
     * Write-only. See the class comment on why reading it is a performance bug
     * and not merely inelegant.
     * */
    void* MappedAt(VkDeviceSize offset) const;

    /**
     * Makes CPU writes visible to the GPU. A no-op inside VMA when the memory
     * type turned out to be HOST_COHERENT, which on Android it nearly always is
     * — called unconditionally precisely so the one device where it is not
     * behaves the same.
     * */
    void Flush(VkDeviceSize offset, VkDeviceSize bytes) const;

private:
    Buffer() = default;

    VmaAllocator allocator_ = VK_NULL_HANDLE;
    VkBuffer handle_ = VK_NULL_HANDLE;
    VmaAllocation allocation_ = VK_NULL_HANDLE;
    void* mapped_ = nullptr;
    VkDeviceSize size_ = 0;
    std::string name_;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_BUFFER_H
