#include "FinalPass.h"

#include "Device.h"
#include "ShaderModule.h"
#include "Swapchain.h"

#include <cstring>

namespace putorana::graphics {

namespace {

constexpr const char* kVertexShader = "shaders/fullscreen.vert.spv";
constexpr const char* kFragmentShader = "shaders/composite.frag.spv";

/** True for the _SRGB colour formats the swapchain is willing to choose. */
bool IsSrgb(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_SRGB:
        case VK_FORMAT_B8G8R8A8_SRGB:
            return true;
        default:
            return false;
    }
}

void SetImageBarrier(VkCommandBuffer commandBuffer, VkImage image, VkImageLayout oldLayout,
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
    barrier.image = image;
    barrier.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    barrier.subresourceRange.levelCount = 1;
    barrier.subresourceRange.layerCount = 1;

    VkDependencyInfo dependency{};
    dependency.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO;
    dependency.imageMemoryBarrierCount = 1;
    dependency.pImageMemoryBarriers = &barrier;
    vkCmdPipelineBarrier2(commandBuffer, &dependency);
}

} // namespace

std::unique_ptr<FinalPass> FinalPass::Create(Device& device, VkFormat targetFormat,
                                             std::string& error) {
    auto pass = std::unique_ptr<FinalPass>(new FinalPass());
    pass->handle_ = device.handle();
    pass->push_.srgbTarget = IsSrgb(targetFormat) ? 1 : 0;

    // The stand-in for the camera planes. One texel, cleared to black on first
    // use, and only ever sampled on frames where the mix factor is 1 and its
    // contribution is therefore multiplied away — see EnsurePlaceholder.
    Image::Desc placeholderDesc;
    placeholderDesc.name = "camera placeholder";
    placeholderDesc.extent = {1, 1};
    placeholderDesc.format = VK_FORMAT_R8_UNORM;
    placeholderDesc.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    pass->placeholder_ =
            Image::Create(device.allocator().handle(), pass->handle_, placeholderDesc, error);
    if (pass->placeholder_ == nullptr) {
        return nullptr;
    }

    VkSamplerCreateInfo samplerInfo{};
    samplerInfo.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    // NEAREST because this is a 1:1 copy — source and target are both the
    // swapchain extent, so every sample lands exactly on a texel and linear
    // filtering would be four taps to compute the same value.
    samplerInfo.magFilter = VK_FILTER_NEAREST;
    samplerInfo.minFilter = VK_FILTER_NEAREST;
    samplerInfo.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    samplerInfo.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(pass->handle_, &samplerInfo, nullptr, &pass->sampler_) != VK_SUCCESS) {
        error = "final pass: could not create the sampler";
        return nullptr;
    }

    // 0 is the mesh pass's output, 1 and 2 the camera's luma and chroma planes.
    // Each carries its own sampler, which is why the NEAREST one above can serve
    // the 1:1 copy while the feed's LINEAR one does the rescale.
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 3;
    layoutInfo.pBindings = bindings;
    if (vkCreateDescriptorSetLayout(pass->handle_, &layoutInfo, nullptr, &pass->setLayout_) !=
        VK_SUCCESS) {
        error = "final pass: could not create the descriptor set layout";
        return nullptr;
    }

    pass->descriptorSet_ = device.descriptorPool().Allocate(pass->setLayout_);
    if (pass->descriptorSet_ == VK_NULL_HANDLE) {
        error = "final pass: could not allocate the descriptor set";
        return nullptr;
    }

    // One range covering both stages, matching the single block declared in both
    // shaders: the vertex stage reads the camera UVs and the fragment stage the
    // two flags.
    VkPushConstantRange pushRange{};
    pushRange.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    pushRange.offset = 0;
    pushRange.size = sizeof(PushConstants);

    VkPipelineLayoutCreateInfo pipelineLayoutInfo{};
    pipelineLayoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    pipelineLayoutInfo.setLayoutCount = 1;
    pipelineLayoutInfo.pSetLayouts = &pass->setLayout_;
    pipelineLayoutInfo.pushConstantRangeCount = 1;
    pipelineLayoutInfo.pPushConstantRanges = &pushRange;
    if (vkCreatePipelineLayout(pass->handle_, &pipelineLayoutInfo, nullptr,
                               &pass->pipelineLayout_) != VK_SUCCESS) {
        error = "final pass: could not create the pipeline layout";
        return nullptr;
    }

    const ShaderModule vertex = ShaderModule::Load(pass->handle_, kVertexShader, error);
    if (!vertex.valid()) {
        return nullptr;
    }
    const ShaderModule fragment = ShaderModule::Load(pass->handle_, kFragmentShader, error);
    if (!fragment.valid()) {
        return nullptr;
    }

    // Empty, and that is the point: fullscreen.vert builds its triangle from
    // gl_VertexIndex, so there is no buffer to bind and the draw is
    // vkCmdDraw(cmd, 3, 1, 0, 0).
    VkPipelineVertexInputStateCreateInfo vertexState{};
    vertexState.sType = VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO;

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // No culling: the triangle's winding is whatever gl_VertexIndex produces
    // after the Y flip, and there is no reason to care — there is exactly one
    // triangle and it always faces the viewer.
    raster.cullMode = VK_CULL_MODE_NONE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    const VkDynamicState dynamicStates[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{};
    dynamic.sType = VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO;
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dynamicStates;

    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vertex.handle();
    stages[0].pName = "main";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = fragment.handle();
    stages[1].pName = "main";

    // No depth attachment at all: this covers every pixel unconditionally.
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &targetFormat;

    VkGraphicsPipelineCreateInfo pipelineInfo{};
    pipelineInfo.sType = VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO;
    pipelineInfo.pNext = &renderingInfo;
    pipelineInfo.stageCount = 2;
    pipelineInfo.pStages = stages;
    pipelineInfo.pVertexInputState = &vertexState;
    pipelineInfo.pInputAssemblyState = &assembly;
    pipelineInfo.pViewportState = &viewport;
    pipelineInfo.pRasterizationState = &raster;
    pipelineInfo.pMultisampleState = &multisample;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = pass->pipelineLayout_;

    const VkResult created = vkCreateGraphicsPipelines(pass->handle_, VK_NULL_HANDLE, 1,
                                                       &pipelineInfo, nullptr, &pass->pipeline_);
    if (created != VK_SUCCESS) {
        error = "final pass: vkCreateGraphicsPipelines failed: " + std::to_string(created);
        return nullptr;
    }
    return pass;
}

FinalPass::~FinalPass() {
    if (pipeline_ != VK_NULL_HANDLE) {
        vkDestroyPipeline(handle_, pipeline_, nullptr);
    }
    if (pipelineLayout_ != VK_NULL_HANDLE) {
        vkDestroyPipelineLayout(handle_, pipelineLayout_, nullptr);
    }
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(handle_, setLayout_, nullptr);
    }
    if (sampler_ != VK_NULL_HANDLE) {
        vkDestroySampler(handle_, sampler_, nullptr);
    }
}

void FinalPass::EnsurePlaceholder(const FrameContext& frame) {
    if (placeholderReady_) {
        return;
    }
    placeholderReady_ = true;

    // Cleared rather than merely transitioned. Sampling an image whose contents
    // are undefined is legal and its result is multiplied by zero here anyway,
    // but a texel of garbage in a RenderDoc capture is a lead worth not leaving
    // lying around.
    SetImageBarrier(frame.commandBuffer, placeholder_->handle(), VK_IMAGE_LAYOUT_UNDEFINED,
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_NONE,
                    VK_ACCESS_2_NONE, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT);

    const VkClearColorValue black{};
    VkImageSubresourceRange range{};
    range.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    range.levelCount = 1;
    range.layerCount = 1;
    vkCmdClearColorImage(frame.commandBuffer, placeholder_->handle(),
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &range);

    SetImageBarrier(frame.commandBuffer, placeholder_->handle(),
                    VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_CLEAR_BIT,
                    VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                    VK_ACCESS_2_SHADER_READ_BIT);
}

bool FinalPass::PointAt(const Image& source, VkImageView luma, VkImageView chroma,
                        VkSampler feedSampler) {
    if (source.view() == boundView_ && luma == boundLuma_ && chroma == boundChroma_) {
        return true;
    }
    // The set is bound by frames that may still be in flight, and rewriting one
    // the GPU is reading is undefined. This happens on a resize, and once more
    // when the first camera image replaces the placeholder.
    if (boundView_ != VK_NULL_HANDLE) {
        vkDeviceWaitIdle(handle_);
    }

    VkDescriptorImageInfo imageInfos[3]{};
    imageInfos[0].sampler = sampler_;
    imageInfos[0].imageView = source.view();
    imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[1].sampler = feedSampler;
    imageInfos[1].imageView = luma;
    imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    imageInfos[2].sampler = feedSampler;
    imageInfos[2].imageView = chroma;
    imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkWriteDescriptorSet writes[3]{};
    for (uint32_t i = 0; i < 3; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descriptorSet_;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    vkUpdateDescriptorSets(handle_, 3, writes, 0, nullptr);

    boundView_ = source.view();
    boundLuma_ = luma;
    boundChroma_ = chroma;
    return true;
}

void FinalPass::Render(const FrameContext& frame, const Image& source, const CameraFeed* feed) {
    const bool hasFeed = feed != nullptr && feed->ready();
    if (!hasFeed) {
        // Nothing to sample yet, so the placeholder goes into both feed
        // bindings. The mesh pass cleared opaquely in this case, so the mix
        // factor is 1 everywhere and what is bound cannot show up.
        EnsurePlaceholder(frame);
    }

    const VkImageView luma = hasFeed ? feed->lumaView() : placeholder_->view();
    const VkImageView chroma = hasFeed ? feed->chromaView() : placeholder_->view();
    const VkSampler feedSampler = hasFeed ? feed->sampler() : sampler_;
    if (!PointAt(source, luma, chroma, feedSampler)) {
        return;
    }

    if (hasFeed) {
        std::memcpy(push_.cameraUv, feed->uv(), sizeof(push_.cameraUv));
        push_.vFirst = feed->vFirst() ? 1 : 0;
    }

    const VkExtent2D extent = frame.swapchain->extent();

    VkRenderingAttachmentInfo colorAttachment{};
    colorAttachment.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
    colorAttachment.imageView = frame.swapchain->imageView(frame.imageIndex);
    colorAttachment.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    // DONT_CARE, not CLEAR: the triangle covers every pixel, so clearing first
    // would be writing a full screen of colour that is immediately overwritten.
    colorAttachment.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    colorAttachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;

    VkRenderingInfo rendering{};
    rendering.sType = VK_STRUCTURE_TYPE_RENDERING_INFO;
    rendering.renderArea.extent = extent;
    rendering.layerCount = 1;
    rendering.colorAttachmentCount = 1;
    rendering.pColorAttachments = &colorAttachment;

    vkCmdBeginRendering(frame.commandBuffer, &rendering);

    VkViewport viewport{};
    viewport.width = static_cast<float>(extent.width);
    viewport.height = static_cast<float>(extent.height);
    viewport.maxDepth = 1.0f;
    vkCmdSetViewport(frame.commandBuffer, 0, 1, &viewport);

    VkRect2D scissor{};
    scissor.extent = extent;
    vkCmdSetScissor(frame.commandBuffer, 0, 1, &scissor);

    vkCmdBindPipeline(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_);
    vkCmdBindDescriptorSets(frame.commandBuffer, VK_PIPELINE_BIND_POINT_GRAPHICS, pipelineLayout_,
                            0, 1, &descriptorSet_, 0, nullptr);
    vkCmdPushConstants(frame.commandBuffer, pipelineLayout_,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(push_), &push_);
    // Three vertices, no buffers. The shader builds the triangle from their
    // indices — see fullscreen.vert on why one triangle and not a quad.
    vkCmdDraw(frame.commandBuffer, 3, 1, 0, 0);

    vkCmdEndRendering(frame.commandBuffer);
}

} // namespace putorana::graphics
