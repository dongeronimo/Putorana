#include "ChunkMaterial.h"

#include "Device.h"
#include "ShaderModule.h"

#include <android/log.h>

#include <algorithm>

namespace putorana::graphics {

namespace {

constexpr const char* kLogTag = "ARReconstructor";
// The two lines that make this a different material from FlatColorMaterial.
constexpr const char* kVertexShader = "shaders/mesh_chunk.vert.spv";
constexpr const char* kFragmentShader = "shaders/mesh_chunk.frag.spv";

/**
 * Matches the MaterialParams block in mesh_chunk.frag, std140.
 *
 * `color` at offset 0, `colorMix` at 16 and `srgbTarget` at 20, which is what
 * std140 gives a vec4 followed by a float and an int. The trailing padding is
 * explicit rather than implied so that adding a member cannot silently land
 * in it.
 * */
struct Params {
    glm::vec4 color{1.0f};
    float colorMix = 1.0f;
    int32_t srgbTarget = 1;
    int32_t padding_[2] = {};
};
static_assert(sizeof(Params) == 32, "Params must match the std140 layout in mesh_chunk.frag");

} // namespace

std::unique_ptr<ChunkMaterial> ChunkMaterial::Create(Device& device, const glm::vec4& color,
                                                     std::string& error) {
    auto material = std::unique_ptr<ChunkMaterial>(new ChunkMaterial());
    material->handle_ = device.handle();

    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 0;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;

    VkDescriptorSetLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    layoutInfo.bindingCount = 1;
    layoutInfo.pBindings = &binding;
    if (vkCreateDescriptorSetLayout(material->handle_, &layoutInfo, nullptr,
                                    &material->setLayout_) != VK_SUCCESS) {
        error = "ChunkMaterial: could not create the material set layout";
        return nullptr;
    }

    material->params_ = Buffer::Create(device.allocator().handle(), sizeof(Params),
                                       VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                                       "chunk params", error);
    if (material->params_ == nullptr) {
        return nullptr;
    }

    material->descriptorSet_ = device.descriptorPool().Allocate(material->setLayout_);
    if (material->descriptorSet_ == VK_NULL_HANDLE) {
        error = "ChunkMaterial: could not allocate the material descriptor set";
        return nullptr;
    }

    VkDescriptorBufferInfo bufferInfo{};
    bufferInfo.buffer = material->params_->handle();
    bufferInfo.range = sizeof(Params);

    VkWriteDescriptorSet write{};
    write.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    write.dstSet = material->descriptorSet_;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    write.pBufferInfo = &bufferInfo;
    vkUpdateDescriptorSets(material->handle_, 1, &write, 0, nullptr);

    material->SetColor(color);
    return material;
}

void ChunkMaterial::Write() {
    const Params params{color_, colorMix_, srgbTarget_ ? 1 : 0, {}};
    params_->Write(&params, sizeof(params));
}

ChunkMaterial::~ChunkMaterial() {
    if (setLayout_ != VK_NULL_HANDLE) {
        vkDestroyDescriptorSetLayout(handle_, setLayout_, nullptr);
    }
    // The descriptor set is not freed: it belongs to the pool, which goes whole.
}

void ChunkMaterial::SetColor(const glm::vec4& color) {
    color_ = color;
    Write();
}

void ChunkMaterial::SetColorMix(float mix) {
    colorMix_ = std::clamp(mix, 0.0f, 1.0f);
    Write();
}

void ChunkMaterial::SetSrgbTarget(bool srgb) {
    srgbTarget_ = srgb;
    Write();
}

MaterialPipeline ChunkMaterial::CreatePipeline(const PipelineContext& context,
                                               VertexFormat format) const {
    std::string error;
    // Locals: whichever way this function returns, the modules are destroyed.
    // A pipeline keeps what it needs from them, so they are of no use after.
    const ShaderModule vertex = ShaderModule::Load(context.device, kVertexShader, error);
    if (!vertex.valid()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return {};
    }
    const ShaderModule fragment = ShaderModule::Load(context.device, kFragmentShader, error);
    if (!fragment.valid()) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", error.c_str());
        return {};
    }

    // Not a hard failure, because MeshPass caches whatever this returns and a
    // null pipeline only skips those draws. But it is worth a line: the symptom
    // of drawing chunk geometry through a static-format pipeline is a shader
    // reading an attribute the pipeline never bound, which is undefined values
    // rather than an error, and it looks like broken normals.
    if (format != VertexFormat::Reconstructed) {
        __android_log_print(ANDROID_LOG_WARN, kLogTag,
                            "ChunkMaterial asked for a '%s' pipeline; its shaders declare only "
                            "position, normal and colour",
                            VertexFormatName(format));
    }

    MaterialPipeline result;

    // The order IS the set numbering: frame, object, material.
    const VkDescriptorSetLayout setLayouts[] = {context.frameLayout, context.objectLayout,
                                                setLayout_};
    VkPipelineLayoutCreateInfo layoutInfo{};
    layoutInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    layoutInfo.setLayoutCount = 3;
    layoutInfo.pSetLayouts = setLayouts;
    if (vkCreatePipelineLayout(context.device, &layoutInfo, nullptr, &result.layout) !=
        VK_SUCCESS) {
        return {};
    }

    // Must outlive the vkCreateGraphicsPipelines call below — the CreateInfo it
    // returns is nothing but pointers into it. See VertexInput in Mesh.h.
    const VertexInput vertexInput = VertexInputFor(format);
    const VkPipelineVertexInputStateCreateInfo vertexState = vertexInput.CreateInfo();

    VkPipelineInputAssemblyStateCreateInfo assembly{};
    assembly.sType = VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO;
    assembly.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    // Counts only; the values are dynamic and the pass sets them per frame.
    VkPipelineViewportStateCreateInfo viewport{};
    viewport.sType = VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO;
    viewport.viewportCount = 1;
    viewport.scissorCount = 1;

    VkPipelineRasterizationStateCreateInfo raster{};
    raster.sType = VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO;
    raster.polygonMode = VK_POLYGON_MODE_FILL;
    // Same as every other material, and worth knowing when the reconstruction
    // first renders as nothing: a marching-cubes sign convention opposite to
    // this makes the surface VANISH rather than shade wrongly. See the note at
    // the bottom of mesh_chunk.frag.
    raster.cullMode = VK_CULL_MODE_BACK_BIT;
    raster.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    raster.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo multisample{};
    multisample.sType = VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO;
    multisample.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    VkPipelineDepthStencilStateCreateInfo depth{};
    depth.sType = VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO;
    depth.depthTestEnable = VK_TRUE;
    depth.depthWriteEnable = VK_TRUE;
    depth.depthCompareOp = VK_COMPARE_OP_LESS; // smaller clip z is nearer
    depth.maxDepthBounds = 1.0f;

    VkPipelineColorBlendAttachmentState blendAttachment{};
    blendAttachment.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                                     VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo blend{};
    blend.sType = VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO;
    blend.attachmentCount = 1;
    blend.pAttachments = &blendAttachment;

    // Required of every material by MeshPass: the surface can be resized or
    // rotated under a pipeline built long before, and a baked viewport would
    // then be wrong with no diagnostic.
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

    // Dynamic rendering: no VkRenderPass to be compatible with, so the pipeline
    // declares the attachment formats itself.
    VkPipelineRenderingCreateInfo renderingInfo{};
    renderingInfo.sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO;
    renderingInfo.colorAttachmentCount = 1;
    renderingInfo.pColorAttachmentFormats = &context.colorFormat;
    renderingInfo.depthAttachmentFormat = context.depthFormat;

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
    pipelineInfo.pDepthStencilState = &depth;
    pipelineInfo.pColorBlendState = &blend;
    pipelineInfo.pDynamicState = &dynamic;
    pipelineInfo.layout = result.layout;

    const VkResult created = vkCreateGraphicsPipelines(context.device, VK_NULL_HANDLE, 1,
                                                       &pipelineInfo, nullptr, &result.pipeline);
    if (created != VK_SUCCESS) {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag,
                            "ChunkMaterial: vkCreateGraphicsPipelines failed: %d", created);
        vkDestroyPipelineLayout(context.device, result.layout, nullptr);
        return {};
    }
    return result;
}

} // namespace putorana::graphics
