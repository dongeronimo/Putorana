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
  - Frame ring (command buffers, acquire semaphores, timeline): created with the Device, untouched by resize

# Pieces
## Instance (cpp, putorana::graphics::Instance)
- Encapsulates VkInstance. It tries to be a 1.3 instance and ```std::unique_ptr<Instance> Create``` will fail if that's not possible in the device. 
- Optional support for validation in ```InstanceConfig```. Validation should only be enabled in debug because the way the app is structured the .so with the library is only available in debug. 
- Should be the 1st thing to be created, at the moment it's created at ```JNI_OnLoad```.
- Lives basically forever once created.
- You are NOT the owner of the Instance pointer, even though I give you a naked ptr. Do not delete it, everything depends upon the instance existing.
- Usage:
  - Creation:
  ```
  putorana::graphics::instance_holder::CreateInstance(kEnableValidation);
  ```
  At the earliest opportunity (i'm doing in ```JNI_OnLoad```).
  - Getting the VkInstance:
  ```
  auto instance = putorana::graphics::instance_holder::Get();
  VkInstance vkInstance = instance->handle();
  ```
---  

## Device (cpp, putorana::graphics::Device)
- Owns everything whose life follows the Android surface: the ```ANativeWindow```, the ```VkSurfaceKHR``` built on it, the selected adapter and the ```VkDevice```. It also holds the swapchain and the frame ring.
- Created in ```OnSurfaceCreated``` and destroyed in ```OnSurfaceDestroyed```. That cycle runs many times over one library load. Pressing Home destroys it and coming back builds a new one, all inside the same Activity instance, so nothing that depends on the device can be cached on the Activity.
- All or nothing. If any step fails, the window is released and the object is left empty, so ```HasSurface()``` is the only thing callers need to check.
- The surface has to exist before the device can be picked. ```VK_KHR_android_surface``` has no ```vkGetPhysicalDevice*PresentationSupportKHR```, so the only way to learn which queue family can present is to ask ```vkGetPhysicalDeviceSurfaceSupportKHR``` about a real surface.
- Keeps a copy of the ```VkInstance``` the surface was made from, because ```vkDestroySurfaceKHR``` wants that same instance back.
- Teardown runs inside out: ```vkDeviceWaitIdle```, swapchain, frame ring, ```vkDestroyDevice```, ```vkDestroySurfaceKHR```, and ```ANativeWindow_release``` last of all. The surface was borrowing the window reference this object holds.
- ```volkLoadDevice``` points volk's global ```vk*``` table at this device. Those pointers dangle between ```vkDestroyDevice``` and the next load, which is harmless only because nothing renders while there is no surface. This is also why vulkan_check.cpp uses a local ```VolkDeviceTable``` for its throwaway device.
- Usage:
  ```
  Device& device = putorana::graphics::device_holder::Get();
  ```
  Never construct one. The JNI entry points in native-lib.cpp drive it, and they all arrive on the render thread.

---

## PhysicalDevice (cpp, putorana::graphics::PhysicalDevice)
- Not a resource. A ```VkPhysicalDevice``` is a handle the instance already owns, so this is a value type behind ```std::optional``` rather than a ```unique_ptr``` like Instance.
- ```Select``` walks every adapter and rejects on API version, missing device extensions, missing features, or the absence of a queue family that can both draw and present. When nothing survives it names each candidate with its reason, because "no suitable GPU" is useless in a bug report.
- The required feature list lives in a table keyed by pointer to member. The check that asks whether a feature is supported and the code that switches it on both read from that table. Keeping them as two separate lists is how a renderer ends up enabling something it never verified.
- Nothing goes in that table that a conforming 1.3 device is allowed to refuse. That rule is what keeps the renderer on a single code path, and it cost us bindless: ```VK_EXT_descriptor_indexing``` was promoted into 1.2 core, but every one of its feature bits stayed optional.
- Being core in 1.3 only buys you two things. The entry points exist, and there is no extension string to ask for. The feature itself is still off until device creation switches it on.
- Insists on one queue family that does graphics and present. A split would force ```VK_SHARING_MODE_CONCURRENT``` on every swapchain image or an ownership transfer around each present, and no Android GPU actually splits them.

---

## Swapchain (cpp, putorana::graphics::Swapchain)
- Owns the presentable images, plus one view and one ```renderFinished``` semaphore for each of them.
- Its life is nested inside the surface's, but shorter. A resize or a rotation replaces it while the surface stays where it is.
- ```imageExtent``` is copied from ```currentExtent``` and ```preTransform``` from ```currentTransform```. The spec decides both: behaviour is platform dependent when the extent disagrees, and a transform that does not match makes the presentation engine rotate every frame it shows.
- Pre-rotation is therefore on, which moves the rotation onto us. Any projection matrix added later has to apply ```preTransform()```, and at 90 or 270 degrees the width and height it should reason about are swapped relative to ```extent()```. Nothing reveals this while the frame is only a clear.
- FIFO present mode. Every implementation must support it, and RenderThread already paces the loop to vsync, so mailbox would render frames nobody sees and eat battery for it.
- The ```renderFinished``` semaphores are indexed by swapchain image, not by frame in flight. Nothing ever tells the CPU that the presentation engine has finished waiting on one, so reusing a semaphore across frames races. One per image works because the same image cannot come back out of ```vkAcquireNextImageKHR``` until the engine is done with it.
- ```VK_SHARING_MODE_EXCLUSIVE``` and no ownership transfers, which is what the single queue family from PhysicalDevice buys.

---

## FrameRing (cpp, putorana::graphics::FrameRing)
- The per frame resources that do not depend on the swapchain: command pool, command buffers, the ```imageAvailable``` semaphores and the timeline semaphore. Device lifetime, so a resize leaves all of it alone.
- The timeline replaces what would otherwise be one ```VkFence``` per slot. Frame N signals value N+1, so ```BeginFrame``` waits for N+1 minus the number of frames in flight before handing the slot over. That single wait is the whole CPU throttle.
- ```EndFrame``` may only be called once a submission has actually gone through. Advance without one and the next ```BeginFrame``` waits forever on a value nobody will signal.
- ```imageAvailable``` is binary because it has to be. The WSI rejects timeline semaphores in acquire and present, since the presentation engine sits outside the queue's timeline and there is no value to hand it.

---

## Frame (cpp, putorana::graphics::DrawFrame)
- Free functions, no class. Drawing a frame is the renderer's job, and the renderer is this namespace rather than an object. Wrapping it in a ```class Renderer``` would only be a namespace with worse ergonomics.
- Runs once per vsync on the render thread. ```frameTimeNanos``` is when the frame is meant to be displayed, not when the call began, so animation driven from it stays smooth when a frame lands late.
- Swapchain recreation has two triggers and both matter. ```surfaceChanged``` is the obvious one. The other is ```VK_ERROR_OUT_OF_DATE_KHR``` or ```VK_SUBOPTIMAL_KHR``` coming back from acquire or present, and on Android that is the trigger that fires on rotation, because the compositor can change the surface transform without the window size moving at all.
- Those two results need opposite handling. ```OUT_OF_DATE``` from acquire leaves the semaphore unsignalled, so the frame can be dropped on the spot. ```SUBOPTIMAL``` signals it, so the frame has to be finished normally or that semaphore stays signalled with nobody waiting on it.
- ```vkQueueSubmit2``` signals the binary present semaphore and the timeline value from one array. Before synchronization2 the timeline values lived in a parallel struct whose arrays had to be kept index aligned by hand.
- The body is currently a full screen clear with a colour that sweeps slowly, so a running loop looks different from one that died three seconds ago. Dynamic rendering, so there is no ```VkRenderPass``` and no ```VkFramebuffer``` anywhere in the project.

---

## NativeRenderer (java, dev.dongeronimo.arreconstructor::NativeRenderer)
- Mirrors the c++ public interface.
- Loads the library (triggering JN_OnLoad).

---

## RenderThread (java, dev.dongeronimo.arreconstructor::RenderThread)
- Drives the c++ side of the renderer, using NativeRenderer.
- Owns one thread, all native calls go thru it, so we can skip locking in cpp
- Thread life = surface life
- Comes from the Choreographer, dont use a while(true).