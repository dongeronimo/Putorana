#include "MeshPass.h"

#include "Device.h"
#include "Node.h"
#include "Swapchain.h"

#include <android/log.h>

#include <algorithm>
#include <functional>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";

/** Doubling from here, so a steady scene stops reallocating almost immediately. */
constexpr uint32_t kInitialObjectCapacity = 64;

void SetImageBarrier(VkCommandBuffer commandBuffer, const Image& image, VkImageLayout oldLayout,
                     VkImageLayout newLayout, VkPipelineStageFlags2 srcStage,
                     VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                     VkAccessFlags2 dstAccess) {
    VkImageMemoryBarrier2 barrier{};
    barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
    barrier.srcStageMask = srcStage;
    barrier.srcAccessMask = srcAccess;
    barrier.dstStageMask = dstStage;
    barrier.dstAccessMask = dstAccess;
    barrier.oldLayout = oldLayout;
    barrier.newLayout = newLayout;
    barrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    barrier.image = image.handle();
    barrier.subresourceRange.aspectMask = image.aspect();
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

} // namespace

std::unique_ptr<MeshPass> MeshPass::Create(Device& device, VkFormat colorFormat,
                                           std::string& error) {
    auto pass = std::unique_ptr<MeshPass>(new MeshPass());
    pass->device_ = &device;
    pass->handle_ = device.handle();
    pass->colorFormat_ = colorFormat;

    // D32 first: on this hardware it is universally available and it removes the
    // banding a 24-bit depth shows on a large view distance. D24 is the fallback
    // because the spec promises one of the two and never says which.
    const VkFormat depthCandidates[] = {VK_FORMAT_D32_SFLOAT, VK_FORMAT_X8_D24_UNORM_PACK32};
    pass->depthFormat_ = FirstSupportedDepthFormat(device.physicalDevice().handle(),
                                                   depthCandidates, 2);
    if (pass->depthFormat_ == VK_FORMAT_UNDEFINED) {
        error = "no supported depth attachment format";
        return nullptr;
    }

    if (!pass->CreateLayouts(error) || !pass->CreateFrameResources(error)) {
        return nullptr;
    }
    return pass;
}

MeshPass::~MeshPass() {
    // Both handles per entry: the pass owns what CreatePipeline returned, not
    // the material that built it.
    for (auto& entry : pipelines_) {
        if (entry.second.pipeline != VK_NULL_HANDLE) {
            vkDestroyPipeline(handle_, entry.second.pipeline, nullptr);
        }
        if (entry.second.layout != VK_NULL_HANDLE) {
            vkDestroyPipelineLayout(handle_, entry.second.layout, nullptr);
        }
    }
    if (frameLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(handle_, frameLayout_, nullptr);
    }
    if (objectLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(handle_, objectLayout_, nullptr);
    }
    // Descriptor sets are not freed: they belong to the DescriptorPool, which is
    // destroyed whole. See DescriptorPool.h.
}

bool MeshPass::CreateLayouts(std::string& error) {
    // Set 0: the frame uniform. Visible to both stages: the vertex shader needs
    // view and proj, the fragment shader needs the camera position for specular.
    VkDescriptorSetLayoutBinding frameBinding{};
    frameBinding.binding = 0;
    frameBinding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    frameBinding.descriptorCount = 1;
    frameBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo frameInfo{};
    frameInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    frameInfo.bindingCount = 1;
    frameInfo.pBindings = &frameBinding;
    if (vkCreateDescriptorSetLayout(handle_, &frameInfo, nullptr, &frameLayout_) != VK_SUCCESS) {
        error = "mesh pass: could not create the frame descriptor set layout";
        return false;
    }

    // Set 1: the object array. A STORAGE buffer and not a uniform one because it
    // is indexed at runtime by gl_InstanceIndex and is unbounded; a uniform
    // buffer caps at 16KiB on plenty of Android hardware, which is 128 objects.
    VkDescriptorSetLayoutBinding objectBinding{};
    objectBinding.binding = 0;
    objectBinding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    objectBinding.descriptorCount = 1;
    objectBinding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo objectInfo{};
    objectInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    objectInfo.bindingCount = 1;
    objectInfo.pBindings = &objectBinding;
    if (vkCreateDescriptorSetLayout(handle_, &objectInfo, nullptr, &objectLayout_) != VK_SUCCESS) {
        error = "mesh pass: could not create the object descriptor set layout";
        return false;
    }
    return true;
}

bool MeshPass::CreateFrameResources(std::string& error) {
    DescriptorPool& pool = device_->descriptorPool();
    const VmaAllocator allocator = device_->allocator().handle();

    for (uint32_t i = 0; i < FrameRing::kFramesInFlight; ++i) {
        frameBuffers_[i] = Buffer::Create(allocator, sizeof(FrameData),
                                          VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                          "mesh pass frame " + std::to_string(i), error);
        if (frameBuffers_[i] == nullptr) {
            return false;
        }
        frameSets_[i] = pool.Allocate(frameLayout_);
        if (frameSets_[i] == VK_NULL_HANDLE) {
            error = "mesh pass: could not allocate the frame descriptor set";
            return false;
        }

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = frameBuffers_[i]->handle();
        bufferInfo.range = sizeof(FrameData);

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = frameSets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(handle_, 1, &write, 0, nullptr);
    }

    for (uint32_t i = 0; i < FrameRing::kFramesInFlight; ++i) {
        objectSets_[i] = pool.Allocate(objectLayout_);
        if (objectSets_[i] == VK_NULL_HANDLE) {
            error = "mesh pass: could not allocate the object descriptor set";
            return false;
        }
    }
    return EnsureObjectCapacity(kInitialObjectCapacity);
}

bool MeshPass::EnsureObjectCapacity(uint32_t objectCount) {
    if (objectCount <= objectCapacity_) {
        return true;
    }

    uint32_t capacity = std::max(objectCapacity_, kInitialObjectCapacity);
    while (capacity < objectCount) {
        capacity *= 2;
    }

    // Rewriting a descriptor set the GPU might be reading is undefined, and
    // these sets are bound by frames that may still be in flight. Growing is
    // rare (a doubling) so the blunt wait is the right trade against tracking
    // which sets are live.
    if (objectCapacity_ != 0) {
        vkDeviceWaitIdle(handle_);
    }

    const VkDeviceSize size = static_cast<VkDeviceSize>(capacity) * sizeof(ObjectData);
    for (uint32_t i = 0; i < FrameRing::kFramesInFlight; ++i) {
        std::string error;
        auto buffer = Buffer::Create(device_->allocator().handle(), size,
                                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                                     "mesh pass objects " + std::to_string(i), error);
        if (buffer == nullptr) {
            __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
            return false;
        }
        objectBuffers_[i] = std::move(buffer);

        VkDescriptorBufferInfo bufferInfo{};
        bufferInfo.buffer = objectBuffers_[i]->handle();
        bufferInfo.range = size;

        VkWriteDescriptorSet write{};
        write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        write.dstSet = objectSets_[i];
        write.dstBinding = 0;
        write.descriptorCount = 1;
        write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        write.pBufferInfo = &bufferInfo;
        vkUpdateDescriptorSets(handle_, 1, &write, 0, nullptr);
    }

    objectCapacity_ = capacity;
    return true;
}

bool MeshPass::EnsureTargets(VkExtent2D extent) {
    if (color_ != nullptr && color_->extent().width == extent.width &&
        color_->extent().height == extent.height) {
        return true;
    }
    // Same reasoning as growing the object buffer: the old images may still be
    // read by a frame in flight, and this happens only on a resize.
    if (color_ != nullptr) {
        vkDeviceWaitIdle(handle_);
    }

    const VmaAllocator allocator = device_->allocator().handle();
    std::string error;

    Image::Desc colorDesc;
    colorDesc.name = "mesh pass colour";
    colorDesc.extent = extent;
    colorDesc.format = colorFormat_;
    // SAMPLED because the final pass reads it back as a texture.
    colorDesc.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    colorDesc.aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    color_ = Image::Create(allocator, handle_, colorDesc, error);
    if (color_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return false;
    }

    Image::Desc depthDesc;
    depthDesc.name = "mesh pass depth";
    depthDesc.extent = extent;
    depthDesc.format = depthFormat_;
    // No SAMPLED: nothing reads this depth back yet. Adding TRANSIENT_ATTACHMENT
    // and lazily allocated memory would let a tiler keep it in tile memory and
    // never write it to RAM at all, worth doing the day it is certain nothing
    // samples it.
    depthDesc.usage = VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    depthDesc.aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    depth_ = Image::Create(allocator, handle_, depthDesc, error);
    if (depth_ == nullptr) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return false;
    }

    __android_log_print(ANDROID_LOG_INFO, kLogTag, "mesh pass targets: %ux%u", extent.width,
                        extent.height);
    return true;
}

const MaterialPipeline* MeshPass::PipelineFor(const Material& material, VertexFormat format) {
    // typeid and not a hand-written tag: the key has to be the material's TYPE,
    // and the type is exactly what the language already knows.
    const PipelineKey key{std::type_index(typeid(material)), format};
    const auto found = pipelines_.find(key);
    if (found != pipelines_.end()) {
        return found->second.pipeline != VK_NULL_HANDLE ? &found->second : nullptr;
    }

    PipelineContext context;
    context.device = handle_;
    context.colorFormat = colorFormat_;
    context.depthFormat = depthFormat_;
    context.frameLayout = frameLayout_;
    context.objectLayout = objectLayout_;

    const MaterialPipeline built = material.CreatePipeline(context, format);
    if (built.pipeline == VK_NULL_HANDLE) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "material '%s' failed to build a pipeline for %s meshes; its draws "
                            "will be skipped",
                            key.materialType.name(),
                            VertexFormatName(format));
    }
    // Cached even when it failed, so the log above happens once rather than
    // every frame.
    const auto inserted = pipelines_.emplace(key, built).first;
    return built.pipeline != VK_NULL_HANDLE ? &inserted->second : nullptr;
}

void MeshPass::Collect(Node& node, std::vector<DrawItem>& items, const Node*& cameraNode) {
    if (cameraNode == nullptr && node.camera != nullptr) {
        cameraNode = &node; // first camera found is THE camera
    }

    if (node.renderable.has_value() && node.renderable->DrawnIn(RenderPassBit::Main)) {
        const Renderable& renderable = *node.renderable;
        // Frustum culling belongs right here, before an object costs a pipeline
        // lookup and a slot in the object buffer. Not implemented yet (there is
        // no frustum extractor) and this is the one line it changes.
        if (renderable.material != nullptr) {
            const Material& material = *renderable.material;
            if (const MaterialPipeline* pipeline =
                        PipelineFor(material, renderable.vertexFormat())) {
                DrawItem item;
                item.renderable = &renderable;
                item.material = &material;
                item.pipeline = pipeline;
                item.mesh = &renderable.mesh();
                item.worldMatrix = &node.worldMatrix();
                items.push_back(item);
            }
        }
        // A renderable with no material is silently skipped for now. The
        // TypeScript renderer substitutes a screaming magenta fallback instead,
        // which is the better behaviour and needs a concrete material to exist.
    }

    for (const std::unique_ptr<Node>& child : node.children()) {
        Collect(*child, items, cameraNode);
    }
}

void MeshPass::Render(const FrameContext& frame, Node& root, bool transparentClear) {
    const VkExtent2D extent = frame.swapchain->extent();
    if (!EnsureTargets(extent)) {
        return;
    }

    // ---- 1. collect ----
    // Reads only the cached world matrices, which World::Update closed for this
    // frame. Nothing is computed here.
    items_.clear();
    const Node* cameraNode = nullptr;
    Collect(root, items_, cameraNode);

    if (cameraNode == nullptr) {
        if (!items_.empty() && !warnedAboutCamera_) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "mesh pass: no node in the scene has a camera, nothing will be "
                                "drawn");
            warnedAboutCamera_ = true;
        }
        items_.clear();
    }

    // ---- 2. sort ----
    // By pipeline, then material, then mesh, which is what makes a run of
    // objects sharing all three CONTIGUOUS, and a contiguous run is what
    // becomes one instanced draw.
    //
    // Reordering is safe only because this pass is entirely opaque and the depth
    // test settles visibility. A pass with blending would need the order to be
    // back-to-front, and this sort would be a bug rather than a speed-up.
    //
    // Ordered by pointer, which the WebGPU version cannot do: it assigns
    // first-appearance ids because JavaScript has no ordering on object
    // references. std::less gives a total order over unrelated pointers, so the
    // ids are unnecessary here.
    std::sort(items_.begin(), items_.end(), [](const DrawItem& a, const DrawItem& b) {
        if (a.pipeline != b.pipeline) {
            return std::less<const MaterialPipeline*>{}(a.pipeline, b.pipeline);
        }
        if (a.material != b.material) {
            return std::less<const Material*>{}(a.material, b.material);
        }
        return std::less<const Mesh*>{}(a.mesh, b.mesh);
    });

    // ---- 3. upload ----
    const uint32_t objectCount = static_cast<uint32_t>(items_.size());
    if (!EnsureObjectCapacity(objectCount)) {
        return;
    }

    if (objectCount > 0) {
        objectStaging_.resize(objectCount);
        for (uint32_t i = 0; i < objectCount; ++i) {
            const glm::mat4& model = *items_[i].worldMatrix;
            objectStaging_[i].model = model;
            objectStaging_[i].normalMatrix = glm::transpose(glm::inverse(model));
        }
        objectBuffers_[frame.frameIndex]->Write(objectStaging_.data(),
                                                objectCount * sizeof(ObjectData));
    }

    FrameData frameData;
    // The view is the inverse of the camera node's world matrix: the camera
    // looks down its own -Z, so positioning the node is all there is to it.
    frameData.view = glm::inverse(cameraNode->worldMatrix());
    frameData.projection =
            cameraNode->camera->ProjectionMatrix(extent, frame.swapchain->preTransform());
    frameData.cameraPosition = glm::vec4(glm::vec3(cameraNode->worldMatrix()[3]), 1.0f);
    frameBuffers_[frame.frameIndex]->Write(&frameData, sizeof(frameData));

    // ---- 4. draw ----
    //
    // UNDEFINED discards whatever the target held, which is right because the
    // loadOp clears it anyway, and on a tiler, preserving means reading the
    // whole image back into tile memory first.
    //
    // The source scope still matters even with UNDEFINED: the previous frame's
    // final pass sampled this image, and it may still be running. A barrier's
    // first synchronization scope covers everything earlier in SUBMISSION order,
    // which spans queue submissions, so naming FRAGMENT_SHADER here is what
    // stops this frame from overwriting an image the last one is still reading.
    // It is also why one colour target is enough rather than one per frame.
    SetImageBarrier(frame.commandBuffer, *color_, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT);
    // DEPTH_STENCIL_ATTACHMENT_OPTIMAL and not the depth-only
    // DEPTH_ATTACHMENT_OPTIMAL, even though this image has no stencil aspect.
    // The separate layouts came in with VK_KHR_separate_depth_stencil_layouts
    // and using one requires the separateDepthStencilLayouts feature to be
    // enabled, which PhysicalDevice does not ask for, because the rule there is
    // that nothing goes in the required list unless a conforming 1.3 device
    // cannot refuse it. The combined layout needs no feature and behaves
    // identically for a format that has only depth.
    SetImageBarrier(frame.commandBuffer, *depth_, VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT, VK_ACCESS_2_NONE,
                    VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT,
                    VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = color_->view();
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    // Alpha is the whole message to the final pass: 0 means "nothing was drawn
    // here, show what is behind", and behind is the camera. With no camera there
    // is nothing behind, so the clear is opaque and the final pass's mix keeps
    // this colour throughout, which is how the cornflower field survives on a
    // device with no ARCore, without a second code path anywhere.
    colorAttachment.clearValue.color =
            transparentClear ? VkClearColorValue{{0.0f, 0.0f, 0.0f, 0.0f}}
                             : VkClearColorValue{{0.39f, 0.58f, 0.93f, 1.0f}}; // cornflower blue

    VkRenderingAttachmentInfo depthAttachment{};
    depthAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    depthAttachment.imageView = depth_->view();
    depthAttachment.imageLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    depthAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    // DONT_CARE: nothing reads the depth after this pass. On a tiler that is the
    // difference between writing a full screen of depth out to RAM and never
    // leaving tile memory with it.
    depthAttachment.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    depthAttachment.clearValue.depthStencil.depth = 1.0f; // far plane

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAttachment;
    rendering.pDepthAttachment = &depthAttachment;

    vkCmdBeginRendering(frame.commandBuffer, &rendering);

    // Dynamic, and required to be so by every material; see Material.h. A baked
    // viewport would be wrong after the first rotation, with no diagnostic.
    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    const MaterialPipeline* boundPipeline = nullptr;
    const Material* boundMaterial = nullptr;
    size_t start = 0;
    while (start < items_.size()) {
        const DrawItem& first = items_[start];

        size_t end = start + 1;
        while (end < items_.size() && items_[end].pipeline == first.pipeline &&
               items_[end].material == first.material && items_[end].mesh == first.mesh) {
            ++end;
        }

        if (first.pipeline != boundPipeline) {
            vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                              first.pipeline->pipeline);
            // Sets 0 and 1 are rebound here rather than once before the loop.
            // Binding them once would work only if every material's pipeline
            // layout were compatible for those sets, which is true today and is
            // the kind of invariant that breaks silently the day a material adds
            // a push constant. One extra call per DISTINCT PIPELINE (a handful
            // per frame after the sort) buys not having to rely on it.
            const VkDescriptorSet sets[] = {frameSets_[frame.frameIndex],
                                            objectSets_[frame.frameIndex]};
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    first.pipeline->layout, kFrameSet, 2, sets, 0, nullptr);
            boundPipeline = first.pipeline;
            // Binding with a different layout disturbs the higher-numbered sets,
            // so set 2 has to go back on even if the material did not change.
            boundMaterial = nullptr;
        }

        if (first.material != boundMaterial) {
            const VkDescriptorSet materialSet = first.material->descriptorSet();
            vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                    first.pipeline->layout, kMaterialSet, 1, &materialSet, 0,
                                    nullptr);
            boundMaterial = first.material;
        }

        const Mesh& mesh = *first.mesh;
        const uint32_t indexCount = mesh.indexCount(frame.frameIndex);
        if (indexCount > 0) {
            mesh.Bind(frame.commandBuffer, frame.frameIndex);
            // firstInstance = start: the run's objects occupy slots start..end-1
            // of the buffer written above, in this exact order, and
            // gl_InstanceIndex is defined as firstInstance plus the instance
            // counter. That identity is the whole instancing scheme.
            vkCmdDrawIndexed(frame.commandBuffer, indexCount,
                             static_cast<uint32_t>(end - start), 0, 0,
                             static_cast<uint32_t>(start));
        }
        start = end;
    }

    vkCmdEndRendering(frame.commandBuffer);

    // Hand the colour off to whatever composites it.
    SetImageBarrier(frame.commandBuffer, *color_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                    VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT,
                    VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
                    VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_READ_BIT);
}

} // namespace putorana::graphics
