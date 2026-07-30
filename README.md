# Putorana
AR real space reconstruction rendered with vulkan, for android.

- Why this name?
I like to name my vulkan projects with volcanos or flood basalts. Putorana is the main flood basalt plateau of the Siberian Traps (https://en.wikipedia.org/wiki/Putorana_Plateau)

- Mission
To reconstruct the real world into the virtual world so that I can use this data to do useful things. The sample will be a simple ship game where the obstacles will be shoals and islands.

- Technologies
  - OpenChisel: cpu-based space reconstructor.
  - Vulkan 1.3: more modern version of vulkan API to do rendering.

---
# Code Convention
- Cpp/H files like this: ```VkContext.cpp``` / ```VkContext.h```.
- Class named like this: ```class FooBar ```.
- Java-style blocks: ```if (lorenIpsun) {```
---

# Graphics Architecture
- Lifecycles:
  - Instance + debug messager: created once when lib loads
  - Surface + Physical Device + Device: created during Surface Created
  - Swapchain: created/recreated during Surface Changed


# Instance
- Encapsulates VkInstance. It tries to be a 1.3 instance and ```std::unique_ptr<Instance> Create``` will fail if that's not possible in the device. 
- Optional support for validation in ```InstanceConfig```. Validation should only be enabled in debug because the way the app is structured the .so with the library is only available in debug. 
- Should be the 1st thing to be created, at the moment it's created at ```JNI_OnLoad```.
- Lives basically forever once created. 


  