#ifndef PUTORANA_GRAPHICS_SHADERMODULE_H
#define PUTORANA_GRAPHICS_SHADERMODULE_H

#include "volk.h"

#include <string>

namespace putorana::graphics {

/**
 * A VkShaderModule loaded from a .spv in the APK.
 *
 * There is no shader compilation in this app. Every module is built ahead of
 * time by tools/compile_shaders.py, from the GLSL in assets/shaders, and lands
 * in app/src/main/assets/shaders as finished SPIR-V. So a shader that changed
 * and was not recompiled is packaged as the previous version — which is the one
 * failure mode of this arrangement, and the reason that script exits non-zero.
 *
 * Move-only RAII rather than a load-and-destroy pair of calls, because of where
 * it is used: a material's CreatePipeline loads two modules, then has half a
 * dozen early returns before vkCreateGraphicsPipelines. Every one of them would
 * leak.
 *
 * Short-lived by design. A module is only needed while a pipeline is being
 * built — the pipeline keeps whatever it needs from it — so the natural shape is
 * a local inside CreatePipeline that goes out of scope on the way out.
 * */
class ShaderModule {
public:
    /**
     * `assetPath` is relative to the assets root, e.g.
     * "shaders/mesh_flat.vert.spv". Returns an empty module with the reason in
     * `error`, so callers check valid() rather than a separate bool.
     * */
    static ShaderModule Load(VkDevice device, const std::string& assetPath, std::string& error);

    ShaderModule() = default;
    ~ShaderModule();

    ShaderModule(ShaderModule&& other) noexcept;
    ShaderModule& operator=(ShaderModule&& other) noexcept;

    ShaderModule(const ShaderModule&) = delete;
    ShaderModule& operator=(const ShaderModule&) = delete;

    VkShaderModule handle() const { return handle_; }
    bool valid() const { return handle_ != VK_NULL_HANDLE; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkShaderModule handle_ = VK_NULL_HANDLE;
};

} // namespace putorana::graphics

#endif //PUTORANA_GRAPHICS_SHADERMODULE_H
