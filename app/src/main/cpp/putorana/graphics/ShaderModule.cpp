#include "ShaderModule.h"

#include "putorana/Assets.h"

#include <utility>

namespace putorana::graphics {

ShaderModule ShaderModule::Load(VkDevice device, const std::string& assetPath,
                                std::string& error) {
    ShaderModule module;

    const std::vector<uint8_t> bytes = assets::Read(assetPath, error);
    if (bytes.empty()) {
        return module;
    }
    if (bytes.size() % 4 != 0) {
        // SPIR-V is a stream of 32-bit words, so this is not a rounding
        // question: a size that is not a multiple of four means the file is not
        // SPIR-V, or is truncated.
        error = "'" + assetPath + "' is " + std::to_string(bytes.size()) +
                " bytes, not a multiple of 4 — not a SPIR-V module";
        return module;
    }

    VkShaderModuleCreateInfo info{};
    info.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    info.codeSize = bytes.size(); // in BYTES, unlike pCode which is in words
    // The cast is safe: the vector's storage comes from operator new, which is
    // aligned for any fundamental type, and Vulkan requires only 4.
    info.pCode = reinterpret_cast<const uint32_t*>(bytes.data());

    const VkResult result = vkCreateShaderModule(device, &info, nullptr, &module.handle_);
    if (result != VK_SUCCESS) {
        error = "vkCreateShaderModule failed for '" + assetPath + "': " + std::to_string(result);
        module.handle_ = VK_NULL_HANDLE;
        return module;
    }
    module.device_ = device;
    return module;
}

ShaderModule::~ShaderModule() {
    if (handle_ != VK_NULL_HANDLE) {
        vkDestroyShaderModule(device_, handle_, nullptr);
    }
}

ShaderModule::ShaderModule(ShaderModule&& other) noexcept
    : device_(other.device_), handle_(other.handle_) {
    other.handle_ = VK_NULL_HANDLE;
}

ShaderModule& ShaderModule::operator=(ShaderModule&& other) noexcept {
    if (this != &other) {
        if (handle_ != VK_NULL_HANDLE) {
            vkDestroyShaderModule(device_, handle_, nullptr);
        }
        device_ = other.device_;
        handle_ = other.handle_;
        other.handle_ = VK_NULL_HANDLE;
    }
    return *this;
}

} // namespace putorana::graphics
