#include "Buffer.h"

#include <android/log.h>

#include <cstring>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

} // namespace

std::unique_ptr<Buffer> Buffer::Create(VmaAllocator allocator, VkDeviceSize size,
                                       VkBufferUsageFlags usage, std::string name,
                                       std::string& error) {
    if (size == 0) {
        // vkCreateBuffer rejects it, and a zero sized buffer is always a bug
        // upstream rather than something to tolerate.
        error = "buffer '" + name + "': size is zero";
        return nullptr;
    }

    auto buffer = std::unique_ptr<Buffer>(new Buffer());
    buffer->allocator_ = allocator;
    buffer->name_ = std::move(name);
    buffer->size_ = size;

    VkBufferCreateInfo bufferInfo{};
    bufferInfo.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bufferInfo.size = size;
    bufferInfo.usage = usage;
    // One queue family for everything (see PhysicalDevice), so there is no
    // ownership to share and no transfer to write.
    bufferInfo.sharingMode = VK_SHARING_MODE_EXCLUSIVE;

    VmaAllocationCreateInfo allocInfo{};
    // AUTO lets VMA pick the memory type from how the buffer is used rather than
    // from a hardcoded property mask. Combined with SEQUENTIAL_WRITE below, on a
    // unified memory device it picks DEVICE_LOCAL|HOST_VISIBLE — the whole point.
    allocInfo.usage = VMA_MEMORY_USAGE_AUTO;
    // The two halves of the contract described in Buffer.h: the CPU writes this
    // and never reads it, and the mapping is permanent so no vmaMapMemory /
    // vmaUnmapMemory pair is needed around every update.
    allocInfo.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT |
                      VMA_ALLOCATION_CREATE_MAPPED_BIT;

    VmaAllocationInfo allocationResult{};
    const VkResult result = vmaCreateBuffer(allocator, &bufferInfo, &allocInfo, &buffer->handle_,
                                            &buffer->allocation_, &allocationResult);
    if (result != VK_SUCCESS) {
        error = "vmaCreateBuffer failed for '" + buffer->name_ + "': " + std::to_string(result);
        return nullptr;
    }

    buffer->mapped_ = allocationResult.pMappedData;
    if (buffer->mapped_ == nullptr) {
        // MAPPED_BIT was asked for and honoured on any memory type VMA can pick
        // under HOST_ACCESS_SEQUENTIAL_WRITE, so this cannot legitimately
        // happen. Catch it here rather than as a null dereference in Write.
        error = "buffer '" + buffer->name_ + "': allocation came back unmapped";
        return nullptr;
    }

    // VMA copies the string, so it does not need name_ to stay alive — but
    // name_ is kept anyway for the error messages above.
    vmaSetAllocationName(allocator, buffer->allocation_, buffer->name_.c_str());
    return buffer;
}

Buffer::~Buffer() {
    if (handle_ != VK_NULL_HANDLE) {
        // Unmaps as part of the destroy, because the allocation was created with
        // MAPPED_BIT rather than mapped by hand.
        vmaDestroyBuffer(allocator_, handle_, allocation_);
    }
}

bool Buffer::Write(const void* data, VkDeviceSize bytes, VkDeviceSize offset) {
    if (bytes == 0) {
        return true;
    }
    if (data == nullptr || offset > size_ || bytes > size_ - offset) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "buffer '%s': refused a write of %llu bytes at offset %llu into a "
                            "buffer of %llu",
                            name_.c_str(), static_cast<unsigned long long>(bytes),
                            static_cast<unsigned long long>(offset),
                            static_cast<unsigned long long>(size_));
        return false;
    }

    std::memcpy(static_cast<uint8_t*>(mapped_) + offset, data, bytes);
    Flush(offset, bytes);
    return true;
}

void* Buffer::MappedAt(VkDeviceSize offset) const {
    return static_cast<uint8_t*>(mapped_) + offset;
}

void Buffer::Flush(VkDeviceSize offset, VkDeviceSize bytes) const {
    vmaFlushAllocation(allocator_, allocation_, offset, bytes);
}

} // namespace putorana::graphics
