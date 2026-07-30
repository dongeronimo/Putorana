#include "Instance.h"

#include <android/log.h>

#include <algorithm>
#include <iterator>
#include <sstream>

namespace putorana::graphics {

namespace {

// Same tag the rest of the native code logs under, so one logcat filter shows
// everything: our messages and the validation layer's.
constexpr const char* kLogTag = "ARReconstructor";

constexpr const char* kValidationLayerName = "VK_LAYER_KHRONOS_validation";

/** Formats a packed Vulkan version (major.minor.patch) into "x.y.z". */
std::string FormatVersion(uint32_t version) {
    std::ostringstream out;
    out << VK_API_VERSION_MAJOR(version) << '.'
        << VK_API_VERSION_MINOR(version) << '.'
        << VK_API_VERSION_PATCH(version);
    return out.str();
}

const char* ResultName(VkResult result) {
    switch (result) {
        case VK_SUCCESS: return "VK_SUCCESS";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "VK_ERROR_OUT_OF_HOST_MEMORY";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "VK_ERROR_OUT_OF_DEVICE_MEMORY";
        case VK_ERROR_INITIALIZATION_FAILED: return "VK_ERROR_INITIALIZATION_FAILED";
        case VK_ERROR_LAYER_NOT_PRESENT: return "VK_ERROR_LAYER_NOT_PRESENT";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "VK_ERROR_EXTENSION_NOT_PRESENT";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "VK_ERROR_INCOMPATIBLE_DRIVER";
        default: return "unexpected VkResult";
    }
}

bool Contains(const std::vector<std::string>& names, const char* name) {
    return std::find(names.begin(), names.end(), name) != names.end();
}

std::vector<std::string> EnumerateLayerNames() {
    uint32_t count = 0;
    if (vkEnumerateInstanceLayerProperties(&count, nullptr) != VK_SUCCESS) {
        return {};
    }
    std::vector<VkLayerProperties> properties(count);
    if (vkEnumerateInstanceLayerProperties(&count, properties.data()) != VK_SUCCESS) {
        return {};
    }

    std::vector<std::string> names;
    names.reserve(properties.size());
    for (const VkLayerProperties& layer : properties) {
        names.emplace_back(layer.layerName);
    }
    return names;
}

/**
 * Appends the extensions exposed by layerName into out, skipping duplicates.
 * Pass nullptr for the loader plus implicit layers.
 * */
void AppendExtensionNames(const char* layerName, std::vector<std::string>& out) {
    uint32_t count = 0;
    if (vkEnumerateInstanceExtensionProperties(layerName, &count, nullptr) != VK_SUCCESS) {
        return;
    }
    std::vector<VkExtensionProperties> properties(count);
    if (vkEnumerateInstanceExtensionProperties(layerName, &count, properties.data()) != VK_SUCCESS) {
        return;
    }
    for (const VkExtensionProperties& extension : properties) {
        if (!Contains(out, extension.extensionName)) {
            out.emplace_back(extension.extensionName);
        }
    }
}

VKAPI_ATTR VkBool32 VKAPI_CALL DebugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT types,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* /*userData*/) {
    int priority = ANDROID_LOG_INFO;
    if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT) {
        priority = ANDROID_LOG_ERROR;
    } else if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT) {
        priority = ANDROID_LOG_WARN;
    }

    const char* kind = "general";
    if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT) {
        kind = "validation";
    } else if (types & VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT) {
        kind = "performance";
    }

    __android_log_print(priority, kLogTag, "[vk/%s] %s: %s", kind,
                        data->pMessageIdName != nullptr ? data->pMessageIdName : "?",
                        data->pMessage != nullptr ? data->pMessage : "");

    // VK_FALSE: never abort the call that produced the message.
    return VK_FALSE;
}

VkDebugUtilsMessengerCreateInfoEXT MakeDebugMessengerInfo() {
    VkDebugUtilsMessengerCreateInfoEXT info{};
    info.sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT;
    // VERBOSE and INFO flood logcat hard enough that the ring buffer starts
    // dropping lines, which hides the messages that matter. WARNING is where
    // actionable output begins.
    info.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT |
                           VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
    info.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
                       VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
    info.pfnUserCallback = DebugCallback;
    return info;
}

} // namespace

std::unique_ptr<Instance> Instance::Create(const InstanceConfig& config, std::string& error) {
    // 1. dlopen libvulkan.so and load the handful of entry points that exist
    // before any instance does: vkCreateInstance and the vkEnumerateInstance*
    // family.
    if (volkInitialize() != VK_SUCCESS) {
        error = "volk: no Vulkan loader on this device (libvulkan.so not found)";
        return nullptr;
    }

    // 2. Find the loader's ceiling before asking for anything. volkGetInstanceVersion
    // is safe on a 1.0 loader, where vkEnumerateInstanceVersion does not exist at
    // all and calling it through volk would dereference a null pointer.
    const uint32_t loaderVersion = volkGetInstanceVersion();
    const uint32_t apiVersion = std::min(loaderVersion, config.apiVersion);
    if (apiVersion < config.apiVersion) {
        std::ostringstream out;
        out << "Vulkan: loader offers " << FormatVersion(loaderVersion)
            << ", this build needs " << FormatVersion(config.apiVersion);
        error = out.str();
        return nullptr;
    }

    // 3. Layers. Validation is best effort on purpose: the layer ships inside the
    // APK, so a debug build on a machine where nobody copied the .so must still
    // run instead of failing at library load.
    std::vector<const char*> layers;
    bool validationEnabled = false;
    if (config.enableValidation) {
        if (Contains(EnumerateLayerNames(), kValidationLayerName)) {
            layers.push_back(kValidationLayerName);
            validationEnabled = true;
        } else {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "%s not found, running without validation. Is it in "
                                "app/src/debug/jniLibs/arm64-v8a/?",
                                kValidationLayerName);
        }
    }

    // 4. Extensions. Two sources have to be queried: the loader with its implicit
    // layers, and the validation layer itself, because VK_EXT_debug_utils and
    // VK_EXT_validation_features are commonly provided by the layer rather than
    // by the loader. Querying only the global list can miss both.
    std::vector<std::string> available;
    AppendExtensionNames(nullptr, available);
    if (validationEnabled) {
        AppendExtensionNames(kValidationLayerName, available);
    }

    std::vector<const char*> extensions;
    // Mandatory: presenting to an ANativeWindow needs both. Nothing else is
    // required at instance level, everything the 1.3 renderer uses is core.
    for (const char* required : {VK_KHR_SURFACE_EXTENSION_NAME,
                                 VK_KHR_ANDROID_SURFACE_EXTENSION_NAME}) {
        if (!Contains(available, required)) {
            error = std::string("Vulkan: missing required instance extension ") + required;
            return nullptr;
        }
        extensions.push_back(required);
    }

    // Gated on the request, not on whether the layer was found: the loader itself
    // implements debug_utils, and it is what gives us vkSetDebugUtilsObjectNameEXT
    // for naming objects. A named buffer in a capture or in a driver message is
    // worth an afternoon of guessing at handle values.
    const bool useDebugUtils =
            config.enableValidation && Contains(available, VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    if (useDebugUtils) {
        extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
    }

    const bool useSyncValidation = validationEnabled && config.enableSyncValidation &&
                                   Contains(available, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    if (useSyncValidation) {
        extensions.push_back(VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME);
    }

    VkApplicationInfo applicationInfo{};
    applicationInfo.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO;
    applicationInfo.pApplicationName = config.applicationName;
    applicationInfo.applicationVersion = config.applicationVersion;
    applicationInfo.pEngineName = config.applicationName;
    applicationInfo.engineVersion = config.applicationVersion;
    applicationInfo.apiVersion = apiVersion;

    VkInstanceCreateInfo instanceInfo{};
    instanceInfo.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO;
    instanceInfo.pApplicationInfo = &applicationInfo;
    instanceInfo.enabledLayerCount = static_cast<uint32_t>(layers.size());
    instanceInfo.ppEnabledLayerNames = layers.data();
    instanceInfo.enabledExtensionCount = static_cast<uint32_t>(extensions.size());
    instanceInfo.ppEnabledExtensionNames = extensions.data();

    // 5. Chaining the messenger info into pNext is what makes the callback cover
    // vkCreateInstance and vkDestroyInstance themselves. Those are exactly the
    // two calls during which no messenger object can exist, so without this a
    // bad instance setup fails silently.
    VkDebugUtilsMessengerCreateInfoEXT debugInfo = MakeDebugMessengerInfo();
    if (useDebugUtils) {
        debugInfo.pNext = instanceInfo.pNext;
        instanceInfo.pNext = &debugInfo;
    }

    constexpr VkValidationFeatureEnableEXT kValidationFeatures[] = {
            VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT,
    };
    VkValidationFeaturesEXT validationFeatures{};
    if (useSyncValidation) {
        validationFeatures.sType = VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT;
        validationFeatures.enabledValidationFeatureCount =
                static_cast<uint32_t>(std::size(kValidationFeatures));
        validationFeatures.pEnabledValidationFeatures = kValidationFeatures;
        validationFeatures.pNext = instanceInfo.pNext;
        instanceInfo.pNext = &validationFeatures;
    }

    VkInstance handle = VK_NULL_HANDLE;
    const VkResult result = vkCreateInstance(&instanceInfo, nullptr, &handle);
    if (result != VK_SUCCESS) {
        std::ostringstream out;
        out << "Vulkan: vkCreateInstance failed with " << ResultName(result);
        error = out.str();
        return nullptr;
    }

    // 6. Load the instance level entry points. "InstanceOnly" deliberately leaves
    // the device level pointers alone: those get loaded by volkLoadDevice once a
    // VkDevice exists, which skips the loader's dispatch-by-handle trampoline on
    // every vkCmd* call. volkLoadInstance would resolve them the slow way.
    volkLoadInstanceOnly(handle);

    std::unique_ptr<Instance> instance(new Instance());
    instance->instance_ = handle;
    instance->apiVersion_ = apiVersion;
    instance->loaderVersion_ = loaderVersion;
    instance->validationEnabled_ = validationEnabled;
    instance->enabledLayers_.assign(layers.begin(), layers.end());
    instance->enabledExtensions_.assign(extensions.begin(), extensions.end());

    // 7. The standalone messenger. It covers everything from here until teardown,
    // where the pNext-chained one takes over again.
    if (useDebugUtils) {
        const VkDebugUtilsMessengerCreateInfoEXT messengerInfo = MakeDebugMessengerInfo();
        const VkResult messengerResult = vkCreateDebugUtilsMessengerEXT(
                handle, &messengerInfo, nullptr, &instance->messenger_);
        if (messengerResult != VK_SUCCESS) {
            __android_log_print(ANDROID_LOG_WARN, kLogTag,
                                "vkCreateDebugUtilsMessengerEXT failed with %s",
                                ResultName(messengerResult));
            instance->messenger_ = VK_NULL_HANDLE;
        }
    }

    return instance;
}

Instance::~Instance() {
    // Reverse creation order. The messenger is owned by the instance so it has to
    // go first, and with it any chance of hearing about problems during
    // vkDestroyInstance. That is the other half of why the create info is also
    // chained into VkInstanceCreateInfo::pNext.
    if (messenger_ != VK_NULL_HANDLE) {
        vkDestroyDebugUtilsMessengerEXT(instance_, messenger_, nullptr);
    }
    if (instance_ != VK_NULL_HANDLE) {
        vkDestroyInstance(instance_, nullptr);
    }
}

bool Instance::IsExtensionEnabled(const char* name) const {
    return Contains(enabledExtensions_, name);
}

std::string Instance::Describe() const {
    std::ostringstream out;
    out << "VkInstance OK\n"
        << "Loader API: " << FormatVersion(loaderVersion_) << "\n"
        << "Instance API: " << FormatVersion(apiVersion_) << "\n"
        << "Validation: " << (validationEnabled_ ? "on" : "off") << "\n"
        << "Extensions:";
    for (const std::string& name : enabledExtensions_) {
        out << "\n  " << name;
    }
    return out.str();
}

namespace instance_holder {

namespace {

std::unique_ptr<Instance> g_instance;
std::string g_error;

} // namespace

void CreateInstance(bool enableValidation) {
    InstanceConfig config;
    config.enableValidation = enableValidation;

    g_error.clear();
    g_instance = Instance::Create(config, g_error);

    if (g_instance != nullptr) {
        __android_log_print(ANDROID_LOG_INFO, kLogTag, "%s", g_instance->Describe().c_str());
    } else {
        __android_log_print(ANDROID_LOG_ERROR, kLogTag, "%s", g_error.c_str());
    }
}

void DestroyInstance() {
    g_instance.reset();
}

Instance* Get() {
    return g_instance.get();
}

const std::string& Error() {
    return g_error;
}

} // namespace instance_holder

} // namespace putorana::graphics
