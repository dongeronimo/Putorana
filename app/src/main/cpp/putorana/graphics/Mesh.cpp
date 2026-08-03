#include "Mesh.h"

#include <android/log.h>

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <limits>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/**
 * Regions are padded to this. Nothing in the spec demands it — a vertex binding
 * offset only has to keep each attribute on its own component alignment, and an
 * index binding offset only has to be a multiple of the index size, both of
 * which a stride of 32, 40 or 2 already satisfy. It is here so that adding a
 * format with a 16-byte-aligned attribute later cannot quietly break the offsets
 * of a mesh that was written years earlier.
 * */
constexpr VkDeviceSize kRegionAlignment = 16;

constexpr VkDeviceSize AlignUp(VkDeviceSize value, VkDeviceSize alignment) {
    return (value + alignment - 1) & ~(alignment - 1);
}

uint32_t IndexSizeFor(VkIndexType type) {
    return type == VK_INDEX_TYPE_UINT16 ? 2u : 4u;
}

} // namespace

uint32_t VertexStrideFor(VertexFormat format) {
    switch (format) {
        case VertexFormat::Skinned:
            return sizeof(SkinnedVertex);
        case VertexFormat::PositionNormal:
            return sizeof(PositionNormalVertex);
        case VertexFormat::Static:
            break;
    }
    return sizeof(StaticVertex);
}

const char* VertexFormatName(VertexFormat format) {
    switch (format) {
        case VertexFormat::Skinned:
            return "skinned";
        case VertexFormat::PositionNormal:
            return "position+normal";
        case VertexFormat::Static:
            break;
    }
    return "static";
}

VkPipelineVertexInputStateCreateInfo VertexInput::CreateInfo() const {
    VkPipelineVertexInputStateCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;
    info.vertexBindingDescriptionCount = static_cast<uint32_t>(bindings.size());
    info.pVertexBindingDescriptions = bindings.data();
    info.vertexAttributeDescriptionCount = attributeCount;
    info.pVertexAttributeDescriptions = attributes.data();
    return info;
}

VertexInput VertexInputFor(VertexFormat format) {
    VertexInput input;

    // One binding: the data is interleaved in a single buffer. Splitting
    // position into its own stream would let a tiler's binning pass read 12
    // bytes a vertex instead of the full stride, which is a real win on this
    // hardware — but it is a win worth taking when there is a shadow or
    // depth-only pass to spend it on, and changing it means touching only this
    // function and the vertex structs.
    input.bindings[0].binding = 0;
    input.bindings[0].stride = VertexStrideFor(format);
    input.bindings[0].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;

    input.attributes[0].location = 0;
    input.attributes[0].binding = 0;
    input.attributes[0].format = VK_FORMAT_R32G32B32_SFLOAT;
    input.attributes[0].offset = offsetof(StaticVertex, position);

    input.attributes[1].location = 1;
    input.attributes[1].binding = 0;
    input.attributes[1].format = VK_FORMAT_R32G32B32_SFLOAT;
    input.attributes[1].offset = offsetof(StaticVertex, normal);

    // Position and normal are declared once, above, for every format. They are
    // at the same offsets in all three vertex structs, which is what lets this
    // function share them instead of branching three ways.
    static_assert(offsetof(StaticVertex, position) == offsetof(PositionNormalVertex, position));
    static_assert(offsetof(StaticVertex, normal) == offsetof(PositionNormalVertex, normal));

    if (format == VertexFormat::PositionNormal) {
        // Stops at two. There is no UV in this format, and declaring location 2
        // anyway would have the pipeline read 8 bytes past the end of the last
        // vertex in the buffer.
        input.attributeCount = 2;
        return input;
    }

    input.attributes[2].location = 2;
    input.attributes[2].binding = 0;
    input.attributes[2].format = VK_FORMAT_R32G32_SFLOAT;
    input.attributes[2].offset = offsetof(StaticVertex, uv);

    input.attributeCount = 3;

    // The first three offsets are shared because SkinnedVertex starts with the
    // same three members in the same order. That is the invariant these
    // static_asserts protect: break it and a static shader would silently read
    // the wrong bytes out of a skinned mesh.
    static_assert(offsetof(StaticVertex, position) == offsetof(SkinnedVertex, position));
    static_assert(offsetof(StaticVertex, normal) == offsetof(SkinnedVertex, normal));
    static_assert(offsetof(StaticVertex, uv) == offsetof(SkinnedVertex, uv));

    if (format == VertexFormat::Skinned) {
        // UINT, not UNORM: these are indices and must arrive in the shader as
        // uvec4 with their integer values intact.
        input.attributes[3].location = 3;
        input.attributes[3].binding = 0;
        input.attributes[3].format = VK_FORMAT_R8G8B8A8_UINT;
        input.attributes[3].offset = offsetof(SkinnedVertex, boneIds);

        // Plain floats, straight through. Weights are normalised values, not a
        // packed encoding, so nothing happens to them between the buffer and the
        // shader's vec4.
        input.attributes[4].location = 4;
        input.attributes[4].binding = 0;
        input.attributes[4].format = VK_FORMAT_R32G32B32A32_SFLOAT;
        input.attributes[4].offset = offsetof(SkinnedVertex, weights);

        input.attributeCount = 5;
    }

    return input;
}

std::unique_ptr<Mesh> Mesh::Create(VmaAllocator allocator, const MeshDesc& desc,
                                   std::string& error) {
    auto mesh = std::unique_ptr<Mesh>(new Mesh());
    mesh->name_ = desc.name;
    mesh->format_ = desc.format;
    mesh->storage_ = desc.storage;
    mesh->vertexStride_ = VertexStrideFor(desc.format);

    const bool isMutable = desc.storage == MeshStorage::Mutable;

    // Capacity, not the current counts, drives everything below: an update that
    // grows a mutable mesh must not need a different index type or a bigger
    // region than the one it was built with.
    mesh->vertexCapacity_ = std::max(desc.vertexCapacity, desc.vertexCount);
    mesh->indexCapacity_ = std::max(desc.indexCapacity, desc.indexCount);
    if (mesh->vertexCapacity_ == 0 || mesh->indexCapacity_ == 0) {
        error = "mesh '" + desc.name + "': needs a non-zero vertex and index capacity";
        return nullptr;
    }
    if (!isMutable && (desc.vertices == nullptr || desc.indices == nullptr)) {
        error = "mesh '" + desc.name + "': immutable meshes must be given their data at creation";
        return nullptr;
    }

    // uint16 whenever the largest possible index fits, which for a capacity of
    // 65535 vertices means a maximum index of 65534 — comfortably below the
    // 0xFFFF that primitive restart would claim, and primitive restart is off
    // anyway. Halves index bandwidth on nearly every real mesh.
    mesh->indexType_ =
            mesh->vertexCapacity_ <= 0xFFFFu ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;

    mesh->regionCount_ = isMutable ? FrameRing::kFramesInFlight : 1;
    mesh->vertexRegionSize_ = AlignUp(
            static_cast<VkDeviceSize>(mesh->vertexCapacity_) * mesh->vertexStride_,
            kRegionAlignment);
    mesh->indexRegionSize_ = AlignUp(
            static_cast<VkDeviceSize>(mesh->indexCapacity_) * IndexSizeFor(mesh->indexType_),
            kRegionAlignment);

    // No TRANSFER_DST on either: nothing ever copies into these, the CPU writes
    // them directly. See Buffer.h.
    mesh->vertexBuffer_ =
            Buffer::Create(allocator, mesh->vertexRegionSize_ * mesh->regionCount_,
                           VK_BUFFER_USAGE_VERTEX_BUFFER_BIT, desc.name + " (vertices)", error);
    if (mesh->vertexBuffer_ == nullptr) {
        return nullptr;
    }
    mesh->indexBuffer_ =
            Buffer::Create(allocator, mesh->indexRegionSize_ * mesh->regionCount_,
                           VK_BUFFER_USAGE_INDEX_BUFFER_BIT, desc.name + " (indices)", error);
    if (mesh->indexBuffer_ == nullptr) {
        return nullptr;
    }

    if (desc.vertices != nullptr && desc.indices != nullptr) {
        // Into every region, not just the first. A mutable mesh created with
        // geometry and then updated only occasionally would otherwise draw
        // whatever garbage the untouched regions hold on the frames that land
        // on them.
        for (uint32_t region = 0; region < mesh->regionCount_; ++region) {
            if (!mesh->WriteRegion(region, desc.vertices, desc.vertexCount, desc.indices,
                                   desc.indexCount)) {
                error = "mesh '" + desc.name + "': failed to write the initial geometry";
                return nullptr;
            }
        }
        mesh->ComputeBounds(desc.vertices, desc.vertexCount);
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag,
                        "mesh '%s': %s, %s, %u verts (%u B each), %u indices (%s), %llu KiB",
                        desc.name.c_str(),
                        VertexFormatName(desc.format),
                        isMutable ? "mutable" : "immutable", mesh->vertexCapacity_,
                        mesh->vertexStride_, mesh->indexCapacity_,
                        mesh->indexType_ == VK_INDEX_TYPE_UINT16 ? "u16" : "u32",
                        static_cast<unsigned long long>(
                                (mesh->vertexBuffer_->size() + mesh->indexBuffer_->size()) / 1024));
    return mesh;
}

bool Mesh::WriteRegion(uint32_t region, const void* vertices, uint32_t vertexCount,
                       const uint32_t* indices, uint32_t indexCount) {
    if (vertexCount > vertexCapacity_ || indexCount > indexCapacity_) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "mesh '%s': %u verts / %u indices exceed the capacity of %u / %u",
                            name_.c_str(), vertexCount, indexCount, vertexCapacity_,
                            indexCapacity_);
        return false;
    }

    const VkDeviceSize vertexOffset = static_cast<VkDeviceSize>(region) * vertexRegionSize_;
    if (!vertexBuffer_->Write(vertices, static_cast<VkDeviceSize>(vertexCount) * vertexStride_,
                              vertexOffset)) {
        return false;
    }

    const VkDeviceSize indexOffset = static_cast<VkDeviceSize>(region) * indexRegionSize_;
    if (indexType_ == VK_INDEX_TYPE_UINT32) {
        if (!indexBuffer_->Write(indices, static_cast<VkDeviceSize>(indexCount) * sizeof(uint32_t),
                                 indexOffset)) {
            return false;
        }
    } else {
        // Narrowing to uint16. Written straight into the mapping rather than
        // through a temporary vector: the loop only ever writes, and forward, so
        // it is exactly the access pattern write-combined memory is fast at.
        auto* destination = static_cast<uint16_t*>(indexBuffer_->MappedAt(indexOffset));
        for (uint32_t i = 0; i < indexCount; ++i) {
            destination[i] = static_cast<uint16_t>(indices[i]);
        }
        indexBuffer_->Flush(indexOffset, static_cast<VkDeviceSize>(indexCount) * sizeof(uint16_t));
    }

    indexCounts_[region] = indexCount;
    return true;
}

void Mesh::ComputeBounds(const void* vertices, uint32_t vertexCount) {
    if (vertices == nullptr || vertexCount == 0) {
        bounds_ = Aabb{};
        return;
    }

    glm::vec3 low(std::numeric_limits<float>::max());
    glm::vec3 high(std::numeric_limits<float>::lowest());
    const auto* bytes = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        // Offset 0 in every format, which is the reason position is declared
        // first in both vertex structs.
        glm::vec3 position;
        std::memcpy(&position, bytes + static_cast<size_t>(i) * vertexStride_, sizeof(position));
        low = glm::min(low, position);
        high = glm::max(high, position);
    }
    bounds_ = Aabb{low, high};
}

bool Mesh::Update(uint32_t frameIndex, const void* vertices, uint32_t vertexCount,
                  const uint32_t* indices, uint32_t indexCount) {
    if (storage_ != MeshStorage::Mutable) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "mesh '%s': Update on an immutable mesh", name_.c_str());
        return false;
    }
    if (vertices == nullptr || indices == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "mesh '%s': Update with null data",
                            name_.c_str());
        return false;
    }

    if (!WriteRegion(RegionFor(frameIndex), vertices, vertexCount, indices, indexCount)) {
        return false;
    }
    ComputeBounds(vertices, vertexCount);
    return true;
}

void Mesh::Bind(VkCommandBuffer commandBuffer, uint32_t frameIndex) const {
    const uint32_t region = RegionFor(frameIndex);

    VkBuffer vertices = vertexBuffer_->handle();
    const VkDeviceSize vertexOffset = static_cast<VkDeviceSize>(region) * vertexRegionSize_;
    vkCmdBindVertexBuffers(commandBuffer, 0, 1, &vertices, &vertexOffset);

    vkCmdBindIndexBuffer(commandBuffer, indexBuffer_->handle(),
                         static_cast<VkDeviceSize>(region) * indexRegionSize_, indexType_);
}

} // namespace putorana::graphics
