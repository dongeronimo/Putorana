# Putorana
AR real space reconstruction rendered with vulkan, for android.

- Why this name?
I like to name my vulkan projects with volcanos or flood basalts. Putorana is the main flood basalt plateau of the Siberian Traps (https://en.wikipedia.org/wiki/Putorana_Plateau)

- Mission: 
To reconstruct the real world into the virtual world so that I can use this data to do useful things. The sample will be a simple ship game where the obstacles will be shoals and islands.

- Technologies
  - ARCore: tracking, camera feed and the depth maps the reconstruction is built from.
  - Vulkan 1.3: more modern version of vulkan API to do rendering.
  - The space reconstructor is written here, on the CPU, with OpenChisel as a reference rather than a dependency. Why, and the mathematics it rests on: ```app/src/main/cpp/putorana/recon/README.md```.
  - ARCore 1.54, through its C API: tracking, the camera image, and the projection that matches the sensor that took it.

- Articles: https://dongeronimo.github.io/Putorana/
  What was tried, what worked, and what only looked like it would, one article per problem and keeping the wrong turns in. Sources in ```docs/```, published by GitHub Pages on every push to main. Conventions for adding an article: ```docs/README.md```.

---
# Code Convention
- Cpp/H files like this: ```VkContext.cpp``` / ```VkContext.h```.
- Class named like this: ```class FooBar ```.
- Java-style blocks: ```if (lorenIpsun) {```
---

# Building
```./gradlew :app:assembleDebug``` and nothing else. The pieces that are not obvious:

- **Shaders are compiled by the build, not by the app.** ```compileShaders``` runs before ```preBuild``` and turns ```assets/shaders/*.vert|frag``` into SPIR-V in ```app/src/main/assets/shaders```. A GLSL error fails the build with glslc's own ```file:line``` message. To run it by hand: ```python tools/compile_shaders.py``` (```--force```, ```--clean```, ```--release```, ```--verbose```). Assumes python 3.x.
- The ```.spv``` are gitignored, being derived. A fresh clone gets them on its first build.
- **glslc comes from the NDK version ```app/build.gradle.kts``` pins, ahead of ```ANDROID_NDK_HOME```.** That variable is machine-wide and drifts: on the machine this was written on it pointed three major versions back. Set ```GLSLC``` to override everything.
- Two asset folders, and the difference matters. ```assets/``` at the repo root is where things are AUTHORED: .blend files, .glsl. ```app/src/main/assets/``` is what Gradle packages into the APK. Only outputs belong in the second one, since pointing Gradle at the first would ship the .blend sources.
- **The library must not pick up a "d" suffix.** assimp's CMakeLists does ```SET(CMAKE_DEBUG_POSTFIX "d" CACHE STRING ...)```, and ```FetchContent_MakeAvailable``` runs it in this project's scope BEFORE our ```add_library```. A target reads its ```<CONFIG>_POSTFIX``` from the matching ```CMAKE_``` variable at creation time, so ours silently became ```libarreconstructord.so``` while ```System.loadLibrary("arreconstructor")``` kept asking for the name without it. The result is an ```UnsatisfiedLinkError``` on the first line of MainActivity, naming a library that is right there in the APK. ```set_target_properties(... DEBUG_POSTFIX "")``` fixes it on our target only, leaving assimp its own convention.
- Debug builds ask for ```VK_LAYER_KHRONOS_validation```, which is not part of Android: it has to be present in ```app/src/debug/jniLibs/arm64-v8a/```. Without it the app runs, silently unvalidated.
- ```arm64-v8a``` only. AR does not run on the emulator, so there is no reason to build x86.
- **ARCore arrives in two halves from two places, and only one of them is on Maven.** The library ```libarcore_sdk_c.so``` ships inside the AAR ```com.google.ar:core```; the HEADER is not in that AAR at all and exists only in the ```google-ar/arcore-android-sdk``` repo, tagged per release. So ```arcore_c_api.h``` is vendored in ```app/src/main/cpp/third_party/arcore/include/```, and the README next to it has the upgrade procedure.
- **CMake downloads and unzips the AAR itself, at configure time**, alongside the ```FetchContent``` that already pulls assimp, and points an ```IMPORTED``` target at the ```.so``` inside. It has to: AGP packages an AAR's ```jni/``` payload into the APK but never exposes the path to the native build, so there is nothing for ```IMPORTED_LOCATION``` to point at otherwise. The obvious alternative, a Gradle ```Copy``` task, which is what the SDK's own samples do, forces configuration-time resolution on every build and still leaves the file missing during an Android Studio sync, because sync runs the CMake configure outside the task graph. **A first configure therefore needs network.**
- The ARCore version lives in ONE place, ```gradle/libs.versions.toml```. It feeds both ```implementation(libs.arcore)``` and the ```-DARCORE_VERSION``` handed to CMake, so the ```.so``` linked against and the ```.so``` packaged cannot drift. The vendored header is the one thing nothing in the build checks, and a mismatch links cleanly and then fails at runtime on a missing symbol or a struct whose layout moved, so bumping the version means editing the catalog AND re-fetching the header at the matching tag.
- The header is covered by the ARCore Additional Terms of Service, not by this project's license.

---

# Graphics Architecture
- Lifecycles:
  - Instance + debug messager: created once when lib loads
  - **AR session (```ar::Subsystem```): created once the CAMERA permission is granted, paused/resumed with the ACTIVITY, destroyed only with the process.** Deliberately NOT with the device: destroying an ArSession throws away tracking and every anchor with it, and the device dies on every trip through Home.
  - Surface + Physical Device + Device: created during Surface Created
  - Swapchain: created/recreated during Surface Changed
  - Frame ring (command buffers, acquire semaphores, timeline): created with the Device, untouched by resize
  - Allocator (VMA) + descriptor pool + GPU profiler (its query pools): created with the Device, destroyed with it
  - World (scene tree, assets, render passes, the camera feed's textures): owned by the Device, released FIRST in its teardown
  - Buffers, images, meshes, pipelines, descriptor sets: nested inside the Allocator's and the pool's lives, so they die every time the app goes to background. **Nothing may be loaded once and cached across surfaces**, and nothing may be cached in a ```static```: a static pipeline cache would outlive the device that made it and hand out handles into a dead driver.

- **Those two lifetimes only ever meet inside a frame**, and that is the AR design in one line. The session outlives the renderer, so nothing it produces may be kept: the camera image and the pose are borrowed under lock, consumed by whoever asked for them in that same frame, and let go. Which is what makes a device teardown a non-event for tracking, and a Pause a non-event for the renderer.

- One frame, end to end (```DrawFrame``` in Frame.cpp):
  1. rebuild the swapchain if a resize or rotation invalidated it; on the first frame that has one, build the World;
  2. ```ar::Subsystem::Update```, the session's tick. FIRST, because it BLOCKS (up to ARCore's built-in 66ms) waiting for the next camera image, so doing it later would sleep while holding a swapchain image and a frame slot. It is also the thing the rest of the frame is about;
  3. ```World::Update(dt)``` reads this frame's AR pose onto the camera node, moves nodes, then closes every world matrix top-down. Touches no Vulkan, so it runs before the acquire and has nothing to wait for;
  4. ```FrameRing::BeginFrame``` blocks until the slot being reused has retired, then ```vkAcquireNextImageKHR```;
  5. ```GpuProfiler::BeginFrame``` is the first thing recorded into the command buffer (it records the query pool reset, which must precede every timestamp written into it), then a ```GpuScope``` around the whole frame;
  6. barrier the acquired image into ```COLOR_ATTACHMENT_OPTIMAL```, hand it to ```World::Render```, barrier it into ```PRESENT_SRC_KHR```. The frame loop knows about no render pass;
  7. inside the world: ```CameraFeed``` uploads the camera image (outside any rendering scope, since it records copies and barriers); ```MeshPass``` collects, sorts, uploads and draws into its own targets, clearing to TRANSPARENT black when there is a feed; ```FinalPass``` converts the feed's YUV planes to RGB and mixes the mesh target over them by its alpha, onto the acquired image;
  8. ```vkQueueSubmit2``` signals the present semaphore and the timeline value together, then present.

# Pieces

## Platform and lifetime
*Everything whose life is dictated by Android: the library load, the surface, and the device hanging off it.*

### Instance (cpp, putorana::graphics::Instance)
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

### Device (cpp, putorana::graphics::Device)
- Owns everything whose life follows the Android surface: the ```ANativeWindow```, the ```VkSurfaceKHR``` built on it, the selected adapter and the ```VkDevice```. Hanging off that: the swapchain, the frame ring, the VMA allocator, the descriptor pool, the GPU profiler, and the World.
- The AR session is NOT in that list and must never be. It belongs to the activity, not to this; see ```ar::Subsystem```. Everything below the surface is rebuilt on every return from background; tracking is not.
- It owns the World for one reason, and it is not tidiness. A world's meshes are allocations from the allocator two lines up, so the world MUST be gone before that allocator is, on a teardown path that runs every time the app is backgrounded. Owned anywhere else that is a rule somebody has to remember; owned here it is what the destructor does. ```Device.h``` only forward-declares ```World```, so the layering stays one way.
- Created in ```OnSurfaceCreated``` and destroyed in ```OnSurfaceDestroyed```. That cycle runs many times over one library load. Pressing Home destroys it and coming back builds a new one, all inside the same Activity instance, so nothing that depends on the device can be cached on the Activity.
- All or nothing. If any step fails, the window is released and the object is left empty, so ```HasSurface()``` is the only thing callers need to check.
- The surface has to exist before the device can be picked. ```VK_KHR_android_surface``` has no ```vkGetPhysicalDevice*PresentationSupportKHR```, so the only way to learn which queue family can present is to ask ```vkGetPhysicalDeviceSurfaceSupportKHR``` about a real surface.
- Keeps a copy of the ```VkInstance``` the surface was made from, because ```vkDestroySurfaceKHR``` wants that same instance back.
- Teardown runs strictly inside out: ```vkDeviceWaitIdle```, then world, swapchain, frame ring, descriptor pool, allocator, ```vkDestroyDevice```, ```vkDestroySurfaceKHR```, and ```ANativeWindow_release``` last of all. The world goes first because everything it holds came from the three things after it; the window goes last because the surface was borrowing the reference this object holds.
- ```vmaDestroyAllocator``` asserts in debug when an allocation is still alive. That assert is the leak check for this whole ordering: it fires exactly when something outlived the device it was allocated from.
- ```volkLoadDevice``` points volk's global ```vk*``` table at this device. Those pointers dangle between ```vkDestroyDevice``` and the next load, which is harmless only because nothing renders while there is no surface. This is also why vulkan_check.cpp uses a local ```VolkDeviceTable``` for its throwaway device.
- Usage:
  ```
  Device& device = putorana::graphics::device_holder::Get();
  ```
  Never construct one. The JNI entry points in native-lib.cpp drive it, and they all arrive on the render thread.

---

### PhysicalDevice (cpp, putorana::graphics::PhysicalDevice)
- Not a resource. A ```VkPhysicalDevice``` is a handle the instance already owns, so this is a value type behind ```std::optional``` rather than a ```unique_ptr``` like Instance.
- ```Select``` walks every adapter and rejects on API version, missing device extensions, missing features, or the absence of a queue family that can both draw and present. When nothing survives it names each candidate with its reason, because "no suitable GPU" is useless in a bug report.
- The required feature list lives in a table keyed by pointer to member. The check that asks whether a feature is supported and the code that switches it on both read from that table. Keeping them as two separate lists is how a renderer ends up enabling something it never verified.
- Nothing goes in that table that a conforming 1.3 device is allowed to refuse. That rule is what keeps the renderer on a single code path, and it cost us bindless: ```VK_EXT_descriptor_indexing``` was promoted into 1.2 core, but every one of its feature bits stayed optional.
- Being core in 1.3 only buys you two things. The entry points exist, and there is no extension string to ask for. The feature itself is still off until device creation switches it on.
- Insists on one queue family that does graphics and present. A split would force ```VK_SHARING_MODE_CONCURRENT``` on every swapchain image or an ownership transfer around each present, and no Android GPU actually splits them.

---

### Swapchain (cpp, putorana::graphics::Swapchain)
- Owns the presentable images, plus one view and one ```renderFinished``` semaphore for each of them.
- Its life is nested inside the surface's, but shorter. A resize or a rotation replaces it while the surface stays where it is.
- ```imageExtent``` is copied from ```currentExtent``` and ```preTransform``` from ```currentTransform```. The spec decides both: behaviour is platform dependent when the extent disagrees, and a transform that does not match makes the presentation engine rotate every frame it shows.
- Pre-rotation is therefore on, which moves the rotation onto us. Any projection matrix added later has to apply ```preTransform()```, and at 90 or 270 degrees the width and height it should reason about are swapped relative to ```extent()```. Nothing reveals this while the frame is only a clear.
- FIFO present mode. Every implementation must support it, and RenderThread already paces the loop to vsync, so mailbox would render frames nobody sees and eat battery for it.
- The ```renderFinished``` semaphores are indexed by swapchain image, not by frame in flight. Nothing ever tells the CPU that the presentation engine has finished waiting on one, so reusing a semaphore across frames races. One per image works because the same image cannot come back out of ```vkAcquireNextImageKHR``` until the engine is done with it.
- ```VK_SHARING_MODE_EXCLUSIVE``` and no ownership transfers, which is what the single queue family from PhysicalDevice buys.

---

### FrameRing (cpp, putorana::graphics::FrameRing)
- The per frame resources that do not depend on the swapchain: command pool, command buffers, the ```imageAvailable``` semaphores and the timeline semaphore. Device lifetime, so a resize leaves all of it alone.
- The timeline replaces what would otherwise be one ```VkFence``` per slot. Frame N signals value N+1, so ```BeginFrame``` waits for N+1 minus the number of frames in flight before handing the slot over. That single wait is the whole CPU throttle.
- ```EndFrame``` may only be called once a submission has actually gone through. Advance without one and the next ```BeginFrame``` waits forever on a value nobody will signal.
- ```imageAvailable``` is binary because it has to be. The WSI rejects timeline semaphores in acquire and present, since the presentation engine sits outside the queue's timeline and there is no value to hand it.

---

### Allocator (cpp, putorana::graphics::Allocator)
- The one ```VmaAllocator```. Every ```VkBuffer``` and ```VkImage``` is carved out of it, and it is owned by Device, so it dies and is rebuilt on every trip through Home, and so does everything allocated from it.
- Exists because ```maxMemoryAllocationCount``` is a real limit, routinely 4096 on Android. One ```vkAllocateMemory``` per mesh burns through it. VMA takes big blocks and suballocates.
- Must be created after ```volkLoadDevice```. VMA is built with ```VMA_DYNAMIC_VULKAN_FUNCTIONS```, so the only two entry points it is handed are ```vkGetInstanceProcAddr``` and ```vkGetDeviceProcAddr``` and it fetches the rest through them. Forgetting to fill those two in is the classic volk+VMA crash: a table of nulls, called on the first allocation.
- ```unifiedMemory()``` checks for a memory type that is ```DEVICE_LOCAL``` and ```HOST_VISIBLE``` at once. Every Android GPU has one, because there is a single pool of RAM. It is queried instead of assumed so the day it is false is a line in logcat rather than mysteriously slow uploads.
- ```vmaDestroyAllocator``` asserts in debug when allocations are still alive. That is the leak check: it fires precisely when something outlived the Device it came from.

---

### DescriptorPool (cpp, putorana::graphics::DescriptorPool)
- Where every ```VkDescriptorSet``` comes from. A pool's budget is fixed at creation: guess low and allocation starts failing halfway through loading a scene, guess high and the memory is reserved regardless. Neither is worth getting right, so this grows: when a block refuses with ```OUT_OF_POOL_MEMORY``` or ```FRAGMENTED_POOL```, another block is created and the allocation retried. Any other result is a real out-of-memory and is reported instead.
- Sets are never freed individually, and the pools are deliberately NOT created with ```FREE_DESCRIPTOR_SET_BIT```: not asking for it lets the driver use a plain bump allocator inside each block. Everything dies with the Device anyway, so there is nothing to reclaim in between.

---

## AR
*Where the real world comes in. One namespace that knows ARCore and not Vulkan, one class that knows Vulkan and not ARCore, and the line between them.*

### Subsystem (cpp, putorana::ar::Subsystem)
- ARCore, and nothing else. **Nothing in ```putorana::ar``` includes volk.h and nothing in it may.** What it produces is plain memory and plain numbers; turning those into VkImages is ```CameraFeed```'s job, on the other side of that line. The rule is structural rather than a matter of discipline (a ```VkImage``` member would not compile), so it cannot quietly rot, and it is what keeps the upload-vs-import decision below a change to one file.
- **Its lifetime is the ACTIVITY's, not the surface's**, and that is the whole reason it is a separate object with a separate holder and a separate Kotlin entry point. A Device dies every time the app is backgrounded and takes the World with it; destroying an ArSession throws away tracking, and with it every anchor and everything reconstructed so far. So it sits at the same level as the ```VkInstance```: created once, paused and resumed with the activity, destroyed with the process.
- ```subsystem_holder``` is deliberately NOT ```device_holder```'s shape. That one constructs on first use, which is right for something with no failure mode. Creating a session fails for half a dozen reasons the user needs to be told about (no ARCore APK, an APK too old, an unsupported device, a permission that was never granted), so it is created explicitly and ```Get()``` may answer null forever after. Every caller already handles that, because "no AR" still has to draw something.
- Two threads touch it: Create/Resume/Pause from the UI thread on the activity's lifecycle, Update from the render thread once a frame. ARCore's session is not thread-safe across those, so every entry point takes one mutex. It is uncontended in the steady state; the overlap it exists for is the user pressing Home while a frame is in flight.
- ```AcquireFrame``` hands back a GUARD holding that lock, not a copy of the frame, and that is not over-engineering. ```CameraFrame``` carries pointers into an ```ArImage``` this object owns, and Pause releases it, from the UI thread, at any moment. onPause runs well before the surface is destroyed, so the render thread really is still copying when that happens. The cost is that Pause waits for a memcpy of about half a megabyte; the benefit is that it cannot happen underneath one.
- ```AR_TEXTURE_UPDATE_MODE_EXPOSE_HARDWARE_BUFFER``` is set for a reason that is not the hardware buffer, and nothing here reads one. The DEFAULT mode expects a ```GL_TEXTURE_EXTERNAL_OES``` name handed over by ```ArSession_setCameraTextureName```, and this process has no GL context at all to make one in. Selecting the hardware-buffer mode makes those texture names ignored by the header's own wording, which is exactly the opt-out wanted: the image is then consumed purely on the CPU through ```ArFrame_acquireCameraImage```.
- ```AR_UPDATE_MODE_BLOCKING```, the default, and wanted: it paces the renderer to the camera, which is the rate at which anything on screen can actually change. The wait is capped by ARCore's own 66ms, so it cannot wedge the loop, and it is why the tick belongs beside the world's update rather than inside a render pass.
- **Strides are not widths.** ```yRowStride``` and ```uvRowStride``` are whatever padding the driver chose, both ```>=``` width. One ```width * height``` memcpy is the bug those fields exist to prevent; rows are copied one at a time whenever the stride disagrees.
- YUV_420_888 nominally has three planes, but on Android U and V are almost always two interleaved views of ONE block, two bytes apart. That block goes up verbatim as a two-channel texture. **Which byte is which is detected, not assumed**: NV12 puts U first, NV21 puts V, both occur in the wild, and guessing wrong swaps the reds and blues, which reads as a colour-space bug and sends you looking in the wrong file.
- ```viewToImage``` is an affine BASIS (the images of (0,0), (1,0) and (0,1)) rather than three finished texture coordinates, for two reasons. The three points handed to ARCore stay inside the unit square, so nothing depends on how ```ArFrame_transformCoordinates2d``` treats out-of-domain input. And the space it describes is the VIEW's, which is NOT the space a fullscreen triangle is drawn in whenever the swapchain carries a preTransform; composing the two is the renderer's business, and this namespace does not know Vulkan exists.
- ```SetDisplayGeometry``` takes the DISPLAY rotation (```Surface.ROTATION_*```), a related but different quantity from the swapchain's ```preTransform```. Everything ARCore answers in view coordinates, the UV basis included, is wrong until it has been told the truth.
- The projection is TAKEN, never computed. It comes from the physical sensor's intrinsics and the display geometry ARCore was told about; a hand-built frustum that is close but not equal does not look like a wrong field of view, it looks like objects sliding against the world whenever the phone moves.
- The pose is the DISPLAY-ORIENTED one, and only meaningful while ```tracking```. Before the first localisation, and whenever tracking is lost, the numbers are stale rather than wrong-but-close, and moving a camera to them jumps the whole scene.
- A device that cannot produce CPU images says so ONCE, not sixty times a second, and one set of UVs is logged after each geometry change. A log line per frame is a log with nothing in it.
- ```arcore_check.cpp``` is the "is the ARCore half of this build wired up at all?" button, in the same spirit as ```vulkan_check.cpp``` and equally unused by the UI. It asks ARCore whether it is available, the one native call needing nothing but a JNIEnv and a Context. Reaching an answer AT ALL, even an unsupported-device answer, proves the vendored header matches the linked ```.so```, that the loader found that ```.so``` in the APK, and that the ARCore APK is reachable over the package manager. A false result is therefore not necessarily a broken build, and the report says which of the two it was.

---

### CameraFeed (cpp, putorana::graphics::CameraFeed)
- The ARCore image as two Vulkan textures: luma at full resolution, single channel; chroma at half resolution, two channels interleaved. Sampling both with the same UVs gets 4:2:0's chroma upsampling for free out of the hardware's bilinear filter.
- **Deliberately NOT a pass.** It opens no rendering scope and draws nothing: it makes the bytes samplable and ```FinalPass``` composites them. An earlier version DID own a pass, drawing the feed into the mesh pass's colour target before the geometry; the attachment then had to be stored and loaded back between the two scopes, which on a tiler is a full screen read plus a full screen write, around 16MB a frame at 1080p. Compositing in the pass that was already going to read that attachment costs one texture fetch.
- **An upload, and not the zero-copy import** ARCore offers. Importing its AHardwareBuffer directly is the faster path and it is not the one taken, because of what it drags in: an *external format* known only at runtime, which forces a ```VkSamplerYcbcrConversion```, which must be an immutable sampler baked into a descriptor set layout, which means the pipeline sampling it cannot exist until the first camera frame has arrived. It is also opaque to a graphics debugger, and being able to look at these two textures in RenderDoc is worth more right now than the copy costs.
- That copy is smaller than it sounds: ARCore's CPU image is not the full sensor, commonly 640x480, so about 460KB at 1.5 bytes per pixel, and no colour conversion happens on the CPU at all. The planes go up as they are and the fragment shader does the matrix. **Changing this decision touches this file and nothing else**, which is the entire reason the subsystem does not speak Vulkan.
- Staging buffers are one per frame in flight, for the same reason a Mutable mesh holds one region per slot: the CPU writes while the GPU may still be reading the previous frame's copy.
- ```Upload``` MUST be called outside any rendering scope. It records buffer-to-image copies and image barriers, neither of which is legal between ```vkCmdBeginRendering``` and ```vkCmdEndRendering```.
- A null frame is the ordinary case for the first frames after a resume, and for any frame the camera did not produce an image for. The textures keep what they last held: a repeated frame is far less noticeable than a flash of the clear colour.
- The texture coordinates are COMPOSED here, from two mappings the corners have to survive: swapchain space to view space (the preTransform), then view space to image space (ARCore's basis). Doing the extrapolation to the triangle's off-screen corners on this side makes it exact by construction. **In portrait the preTransform is identity and that first step is the identity function, which is exactly why a bug in it shows up only after the first rotation.**
- ```ArCamera::ProjectionMatrix``` applies the same rotation, in the same direction. Those two have to agree, or the cube and the camera image rotate away from each other.

---

## GPU resources
*The things memory is spent on. All of them die with the Allocator, which dies with the Device.*

### Buffer (cpp, putorana::graphics::Buffer)
- A ```VkBuffer``` plus its VMA allocation, permanently mapped, written straight from the CPU.
- No staging buffer anywhere, and that is the platform rather than a shortcut. Staging exists because a discrete GPU has memory the CPU cannot reach. Here there is one pool of RAM and ```DEVICE_LOCAL|HOST_VISIBLE``` memory types exist, so a staging copy would be memcpy'ing RAM to RAM. No buffer here carries ```TRANSFER_DST```.
- What that memory usually *is*, though, is write-combined and uncached: writes are fast, reads are tens of times slower because every one goes to DRAM. So the mapped pointer is write-only, written forward, never used as scratch and never read back, not even for a read-modify-write. ```VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT``` states that contract to VMA and is what gets the fast memory type back.
- ```Flush``` is called on every write. VMA turns it into nothing when the memory is ```HOST_COHERENT```, which on Android it nearly always is. It is unconditional so the one device where it is not behaves identically.
- ```Write``` is bounds-checked and refuses rather than corrupting whatever VMA suballocated next door.

---

### Image (cpp, putorana::graphics::Image)
- A VMA image plus its one view, the counterpart of Buffer for attachments and textures.
- Never host-mapped, unlike Buffer. OPTIMAL tiling puts the memory layout in the driver's hands, so uploading pixels means a staging buffer and ```vkCmdCopyBufferToImage```. This is the one place "Android has unified memory so no staging is needed" does NOT apply: the obstacle is tiling, not locality.
- Attachments get ```VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT```. Full screen targets are large, tilers often want them on their own, and it stops one from pinning a whole suballocation block alive.
- The depth format is queried, never assumed. The spec guarantees only that at least ONE of ```D32_SFLOAT``` and ```X8_D24_UNORM_PACK32``` supports depth attachment, and never says which.

---

### Mesh (cpp, putorana::graphics::Mesh)
- Indexed geometry on the GPU: one vertex buffer, one index buffer, the counts to draw them with, and a local AABB. Knows nothing about materials, pipelines or the scene: a Mesh is something you bind, not something that draws itself.
- Two independent axes, and keeping them independent is the design. ```VertexFormat``` (Static/Skinned) changes the stride and the vertex input state and nothing else; ```MeshStorage``` (Immutable/Mutable) changes the memory layout and nothing else. Every combination works and neither axis knows the other exists.
- One interleaved vertex buffer. Position is the first member of every format, so bounds, culling and any future position-only pass use one stride and offset 0 regardless of what the mesh is.
- ```StaticVertex``` is 32 bytes, ```SkinnedVertex``` is 52. Weights are ```R32G32B32A32_SFLOAT``` and stay exactly as the exporter produced them: they are normalised floats, not a packed encoding, and nothing happens to them between the buffer and the shader. Joint indices are ```R8G8B8A8_UINT```, which is what glTF itself uses for JOINTS_0 and caps a mesh at 256 joints; the format has mandatory vertex-buffer support, so there is nothing to query at runtime.
- Shader locations are shared by both formats (0 position, 1 normal, 2 uv, 3 boneIds, 4 weights), so a shader written for Static reads a Skinned mesh correctly and just ignores 3 and 4.
- Indices arrive as uint32 from every caller and are narrowed to uint16 when the vertex *capacity* fits, halving index bandwidth on nearly every real mesh. Capacity and not the current count, so a mutable mesh that grows later does not need a different index type than it was built with.
- ```VertexInput``` owns its arrays on purpose. ```VkPipelineVertexInputStateCreateInfo``` is two pointers and two counts, so building it from temporaries hands ```vkCreateGraphicsPipelines``` dangling memory, which shows up as garbage geometry on one driver and works fine on another.
- Mutable does not cost a staging buffer, it costs memory. Unified memory solves the transfer; it does not solve timing. While frame N records on the CPU, the GPU is still reading what frame N-1 bound, so overwriting is a race with no validation message and a symptom (geometry flickering between two shapes) that looks like a bug in whatever generated the vertices. So a Mutable mesh holds one region per frame in flight inside the same allocation.
- That is why ```Bind```, ```Draw``` and ```Update``` all take a frame index: pass ```FrameSlot::index``` and it is right by construction. An Immutable mesh has one region and ignores the argument, so callers never branch on storage type.
- The contract that follows: whoever writes a Mutable mesh writes the whole region it is about to draw, every frame it changes. Data written into one slot is not in the others. Initial data given to ```Create``` goes into all of them, so a mutable mesh that is never updated still draws what it was born with.
- There is no ```Draw()``` here, on purpose. A draw is not a property of the geometry: ```instanceCount``` and ```firstInstance``` come from a run of objects that share a pipeline, a material and this mesh, and only the pass doing the batching knows about that run. A ```Draw()``` on the Mesh would have to take both as arguments anyway, and would hide ```indexCount```, the number the batching loop needs, inside itself, quietly inviting one draw call per object.
- ```Bind``` stays, because it is the one thing that genuinely is mesh-private: which region of the buffer this frame's data lives in. The pass calls ```Bind``` then issues its own ```vkCmdDrawIndexed``` with ```mesh.indexCount(frameIndex)```. A count of zero means an unwritten region, so skip it.
- ```Aabb``` (local space here, world space once a Renderable has transformed it) lives in this header because geometry is where the vocabulary belongs.

---

## The scene
*Pure data and hierarchy. Nothing in this group records a Vulkan command.*

### Node (cpp, putorana::graphics::Node)
- A transform in a hierarchy plus whatever optional components hang off it. A node by itself is a position, a scale and an attitude; what it IS comes from what is attached. Renderable present means it is drawn, camera present means it is a camera, light present means it is a light. A node carrying none is a pure pivot, which is not a degenerate case but the most common one, since every parent is one.
- Two storage strategies, and the split is not taste. ```renderable``` is an ```optional```: concrete, final, small, held inline with no allocation. ```camera``` and ```light``` are ```unique_ptr``` because both are polymorphic bases: the object is really a PerspectiveCamera or an ArCamera, a PointLight or a SpotLight, and ```optional<Base>``` stores a Base by value, slicing ARCore's whole projection off one and a spot's cone angles off the other. Neither would even compile: both constructors are protected precisely so a bare base cannot exist.
- That an ArCamera writes ARCore's pose into this node's transform, rather than keeping a view matrix of its own, is what keeps exactly one answer in the tree to "where is the camera".
- **Rotation is a quaternion inside and unbounded Euler degrees outside.** The quaternion is the source of truth: it feeds the matrices and any future interpolation. The Euler angles are an editable VIEW of it, with Unity's semantics.
- Setting ```eulerAngles``` stores the numbers verbatim, unclamped. Set (500, -720.5, 1234) and read back (500, -720.5, 1234), which is what lets an animation drive an angle continuously through 360 without the value jumping. Setting the quaternion directly makes those raw numbers meaningless, since a quaternion cannot say whether it got there by turning 30 degrees or 390, so the cache is flagged stale and the next read derives canonical angles instead (x in [-90,90], y and z in [-180,180]).
- The Euler convention is degrees, applied Z then X then Y about WORLD axes, so R = Ry * Rx * Rz. Written out as three explicit ```angleAxis``` products rather than through a library helper, because every library spells the order differently and half of them mean intrinsic axes by it. Verified numerically against Ry*Rx*Rz over 20000 random angles (worst element error 1.2e-15) and for round-trip through the canonical extraction.
- Under gimbal lock the extraction pins z to 0 and gives the whole remaining turn to y, the same choice Unity and three.js make. Reading back (90, 15, 0) after setting (90, 35, 20) is correct: it is the same rotation.
- The Euler cache members are ```mutable``` because reading ```eulerAngles()``` on a stale cache derives and stores. That changes the cache, not the rotation: the node's orientation is identical before and after, which is the case ```mutable``` exists for.
- ```position``` and ```scale``` are public and mutable in place, three.js style, because nothing is derived from them. Rotation is not, because the Euler cache is. That asymmetry is the entire reason one is a field and the other a pair of methods.
- **A node owns its children**, and a root is owned by whoever called ```Create```. The constructor is private so a stack-allocated node cannot be parented by accident.
- The alternative, every node a raw pointer, one flat container owning them all, is a real design and a common one, but it needs that container to exist and there is no World class yet. An owning tree needs nothing but itself and fits this app's lifetime exactly: everything dies together when the surface goes, so releasing the root releases the scene, with no traversal and nothing left to leak. It is also what makes destroying a prefab instance one line instead of a walk that erases nodes from a flat list.
- ```SetParent(newParent)``` is the normal way to reparent. It returns false and changes NOTHING in two cases: a cycle, and a node that is still a root, whose ```unique_ptr``` is held by its creator, so there is nobody for the method to take it from. That one is spelled ```newParent.AddChild(std::move(ptr))```, the only form that says what happens to the caller's pointer.
- The cycle check in ```SetParent``` runs BEFORE the detach, deliberately. Leaving it to ```AddChild``` would mean the subtree is already unhooked by the time the refusal happens, so a failed reparent would tear a branch out of the scene and then delete it.
- ```AddChild``` has its own check for the same reason: a subtree that owns itself would never be destroyed, and the destructor would recurse forever if anything tried.
- ```Detach``` is ```[[nodiscard]]``` because dropping the result destroys the subtree. That is a legitimate way to delete a branch, but it should be written on purpose: ```node->Detach().reset()```.
- Neither copyable nor movable. Children hold a raw back-pointer to their parent, so a move would leave every child pointing at a corpse. Copying a subtree is a different operation with different semantics (shared assets, fresh per-object state) and belongs to whatever implements prefabs.
- ```worldMatrix()``` is the cached read, O(1), valid only after ```UpdateWorldMatrices``` has run over the node this frame. ```ComputeWorldMatrix()``` walks up the chain and is always current at O(depth), for use outside the frame loop, such as right after building a tree. The update is top-down so each node composes onto its parent's already-refreshed cache, which is what makes the tree O(n) rather than O(n * depth).
- ```LookAt``` aims the node's -Z, the glTF camera convention, which is what makes the view matrix fall straight out of the inverse of the world matrix. Note this is the OPPOSITE of Unity, whose LookAt aims +Z. It substitutes a different up axis when the requested one is parallel to the aim. Aiming a camera straight down is not exotic, and the degenerate basis would otherwise produce a NaN quaternion that spreads silently through every matrix the node touches.
- ```CopyLocalFrom``` writes the members directly rather than going through the setters, because either setter would destroy half the state: one flattens the raw angles, the other rebuilds the quaternion from them. Copying all three pieces plus the sync flag reproduces the source exactly, whichever way it was last edited.

---

### Renderable (cpp, putorana::graphics::Renderable)
- What a scene node draws: one Mesh, one Material, and the per-object decisions that belong to the object rather than the asset. Two pointers and two fields, and no Vulkan anywhere in it.
- Owns neither the Mesh nor the Material. Those are assets shared by reference (two hundred wall tiles point at one Mesh and one Material between them), and whatever loaded them destroys them, which means dying with the Device.
- Copyable, and the copy IS the prefab clone: pointers shared, per-object state copied. The TypeScript renderer needs a hand-written ```cloneRenderable``` for this and has a bug waiting in it every time a field is added and the clone is not updated. A ```static_assert``` pins the property so a ```unique_ptr``` member cannot creep in and silently break it.
- ```passMask``` is an OR of ```RenderPassBit```, typed as ```uint32_t``` because the OR of two enumerators is not itself an enumerator. Each pass walks the tree and skips whoever lacks its bit, which is how one object is drawn by the main pass and again by every shadow pass.
- ```RenderPassBit::Skinned``` replaces ```Main``` rather than joining it: the per-object descriptor set is a different shape (a pool of bone matrices, not one model matrix), so a mesh cannot be in both. It is also a separate question from ```VertexFormat::Skinned```: the format says what the vertices contain, the bit says who draws them, and a skinned mesh can be handed to the main pass and drawn rigid in its bind pose.
- ```castsShadow``` is on the Renderable and not the Material on purpose. Two objects sharing a material are allowed to disagree, and it is the object that occupies space. The case that forces it is ground clutter: geometry thinner than a shadow-map texel contributes no lighting information and a lot of artefact, and costs a draw in every shadow map. Nothing reads it yet.
- ```material``` starts null: the loader builds renderables from a glTF, which carries none of ours. A pass meeting a null material substitutes a loud fallback rather than skipping the draw, because an object screaming "I have no material" debugs faster than an object that is silently absent.
- ```WorldBounds``` takes the matrix as an argument instead of reaching for a node, which is what keeps the scene graph and the renderer separable. All eight corners, because rotation means the extreme corners of the local box are generally not the extreme corners of the transformed one.

---

### Camera (cpp, putorana::graphics::Camera)
- What a node needs to be a camera: a way to produce a projection matrix. Nothing else is in the base.
- **Abstract, because there are two genuinely different sources for a projection.** A ```PerspectiveCamera``` computes it from a field of view and an aspect ratio, the way every renderer does. An ```ArCamera``` does not compute it at all: it is handed one built from the physical sensor's intrinsics. Inventing a projection instead would put the virtual geometry in a subtly different frustum from the camera image behind it, and that mismatch is not visible as a wrong FOV: it is visible as objects that refuse to stay stuck to the world.
- The VIEW matrix is not here, and never was. It is the inverse of the owning node's world matrix, because a camera looks down its own -Z, the same convention as Light's direction and as glTF's. Keeping it out of this class is what lets a camera be parented to a ship and follow it with no code, and it is also **what lets an ArCamera work with no special case anywhere**: the AR pose is written INTO the node, so the inverse of that node's world matrix already IS ARCore's view matrix.
- A Node holds this by ```unique_ptr``` and not by ```optional```. Polymorphism means a pointer: ```optional<Camera>``` stores a Camera by value and would slice ARCore's whole projection off. Same reasoning as Light, and the same shape.
- ```SurfaceRotation(transform)``` is a free function in this header, and **every projection in the renderer is multiplied by it from the LEFT**, because it acts on clip space, after everything else. The swapchain is created with the surface's ```currentTransform``` (see Swapchain), which is a promise to the presentation engine that the content arrives already rotated, which saves the compositor a full screen blit on every frame, and the price is that keeping the promise is the renderer's job.
- ```CameraFeed``` applies the same rotation to its texture coordinates, for the same reason and in the same direction. Those two have to agree or the virtual content and the camera image end up rotated relative to each other.
- The four matrices are built by hand from exact integers rather than with ```glm::rotate```, because ```cos(pi/2)``` in floating point is -4.4e-8 and not zero. These are permutations of the axes and deserve to be exactly that; a projection that is a hair off square is a needle in a haystack later. Only the four rotations are handled: the mirrored transforms exist in the spec but no Android compositor reports one, and identity is the honest answer to INHERIT.

---

### PerspectiveCamera (cpp, putorana::graphics::PerspectiveCamera)
- The ordinary kind: computes its own projection from a field of view and the framebuffer's aspect ratio. What a non-AR scene uses, and what this world falls back to on a device with no AR session.
- Not a 29-line class like the WebGPU original it is ported from, because three transforms sit between a right-handed world and an Android screen and none of them are optional.
- ```perspectiveRH_ZO``` and not ```glm::perspective```. RH is the glTF convention the whole engine uses; ZO is Vulkan's [0,1] clip depth, which is NOT GLM's default. GLM ships OpenGL's [-1,1] and only switches globally with ```GLM_FORCE_DEPTH_ZERO_TO_ONE```. A global that halves everyone's depth precision if someone drops it is worse than naming the variant at the one call site.
- ```projection[1][1] *= -1``` because Vulkan's NDC +Y points DOWN, unlike OpenGL and WebGPU. Skipping it renders the scene upside down and, worse, reverses triangle winding so back-face culling quietly removes the faces it should keep.
- Pre-rotation is applied from the left: ```SurfaceRotation * proj * view * model```. It goes AFTER the Y flip so the rotation happens in y-down clip space, where a positive angle reads clockwise on screen, which is what ```ROTATE_90``` means in the spec.
- There is no ```aspect``` field. It is derived from the extent inside ```ProjectionMatrix```, because under a 90 or 270 degree rotation the width and height the camera should reason about are SWAPPED relative to the swapchain's, and a settable field is an invitation to compute it from ```extent.width/extent.height``` somewhere else and get exactly that wrong. Getting it wrong looks like a bad FOV, not like a rotation bug.
- ```fovY``` is in DEGREES, because it is a number a human types, the same reason the node's Euler angles are. (Spotlight cone angles are radians because those are read verbatim out of a glTF.)
- ```nearPlane```/```farPlane``` and not ```near```/```far```, which are macros in ```<windef.h>```. Depth precision lives in the RATIO far/near: pushing near from 0.1 to 0.5 buys more than pulling far from 1000 to 200.

---

### ArCamera (cpp, putorana::graphics::ArCamera)
- A camera whose projection comes from ARCore rather than from a field of view. **The matrix is taken, not computed**: it has to match the physical sensor that took the picture behind it: its focal length, its principal point, the crop ARCore applied to fit the viewport. Reproducing that from a fovY means agreeing with ARCore's own arithmetic to the last decimal. Where it disagrees, virtual objects do not look mis-framed; they look like they are sliding over the world, and only while the phone moves.
- **```SetFrame``` writes the pose onto the owning NODE, not into this class.** Two things fall out of that, both wanted: the view matrix needs no special case, since MeshPass already builds it as the inverse of the camera node's world matrix and ARCore's view matrix is by definition the inverse of the pose just written there; and the scene graph tells the truth, so anything that wants to know where the device is, or gets parented to it later, reads the node like any other.
- Call it from the world's Update, BEFORE ```World::Update``` closes the world matrices. After, and the camera's transform lands a frame late and every virtual object lags the picture by one frame.
- A frame whose tracking state is not TRACKING is ignored for the pose (it is stale rather than approximately right), but its projection is still taken, because that depends on the display geometry rather than on tracking.
- ARCore's C API is OpenGL-shaped throughout: +Y up, clip Z in [-1,1], column major. Both conversions happen here, once per frame, rather than being left for a shader to guess at. Column-major floats go straight into a ```glm::mat4```: same memory order, so it is a copy and not a transpose.
- Clip Z from [-1,1] to [0,1] is ```z' = (z + w) / 2```, written as a row operation over every column because that is literally all it is. Skipping it does not black the screen: it halves the usable depth range and quietly clips everything in front of the midpoint, which reads as geometry disappearing when it gets close.
- The Y flip negates the **whole row 1, not just ```[1][1]```**. ARCore's projection carries a principal point offset, so unlike a textbook perspective matrix it has terms off the diagonal, and negating only the diagonal one leaves the image off-centre by however far the sensor's optical axis is from the middle.
- ARCore's quaternion order is qx, qy, qz, qw; ```glm::quat```'s constructor takes w FIRST. Getting this wrong produces a rotation that looks plausible and is wrong in a way no still frame reveals.
- ```ProjectionMatrix``` does no aspect ratio arithmetic at all, and that is the point: ARCore was told the viewport's shape through ```ArSession_setDisplayGeometry``` and built the matrix for it, so recomputing an aspect here would be a second opinion, and two opinions about one frustum is exactly how the content stops matching the picture. The pre-rotation is still applied, for the reason in Camera.
- ```nearPlane```/```farPlane``` are in METRES, and really are: this is the one camera in the engine whose scene has a physical scale. They must be pushed to ```ar::Subsystem::SetClipPlanes```: ARCore builds the projection, so setting them here alone does nothing at all.

---

### Light (cpp, putorana::graphics::Light)
- A light source as scene data, hanging off a Node. No Vulkan in it at all, not even an include. Whatever gathers lights each frame reads the fields and packs them.
- Position and direction are deliberately absent. Both come from the owning node's world matrix: position is its translation, direction is its -Z, the same convention as Camera and glTF. Storing a direction here would duplicate the node's rotation in a second place, and two copies of a rotation drift, and a spotlight could no longer just be parented to a torch.
- ```LightType``` is a plain field, not a virtual call. The consumer buckets lights by type and writes three differently shaped GPU arrays, so it is one switch at collection time then a ```static_cast```. Paying for virtual dispatch to ask "what are you" would be backwards for a data-oriented job.
- The copy constructor and assignment are PROTECTED on the base. Subclasses stay freely copyable, but assigning a ```SpotLight``` through a ```Light&```, which would slice the cone angles off, will not compile.
- Colour is LINEAR RGB. It gets multiplied by intensity and summed with other lights, and both only mean anything in linear space; the sRGB conversion happens once, at the end of the frame.
- ```intensity``` means different things per type, which is the honest description rather than a leak: numerator of the linear ```intensity/distance``` falloff for Point and Spot, a plain multiplier for Directional, which does not attenuate because it is infinitely far away.
- Spot cone angles are in RADIANS while ```Camera::fovY``` is in DEGREES. Deliberate, and it comes from where the numbers originate: cone angles are read verbatim out of a glTF's ```KHR_lights_punctual```, which specifies radians; a field of view is a number a human types.
- ```cullable``` exists so a sun can opt out of frustum culling and not blink off when it leaves the frame. Nothing reads it yet.
- Being polymorphic is what forces ```Node::light``` to be a ```unique_ptr``` while ```renderable``` and ```camera``` are ```std::optional```. ```optional<Light>``` stores a Light by value and would slice a SpotLight's cone angles off. It would not even compile, since the constructor is protected precisely so a bare Light cannot exist. The storage choice follows from the type, not from taste.

---

### World (cpp, putorana::graphics::World)
- A scene plus the way that scene is drawn. It owns four things and that ownership IS the class: the tree rooted at ROOT, the assets in it (meshes, materials), its own sequence of render passes, and the frame's update.
- The pass chain belongs to the world and not to the frame loop because each world draws differently: one is mesh+final, another cubemap+shadow+gbuffer, a volume renderer is something else again. ```Frame.cpp``` does acquire, layout transitions, submit and present, and knows about no pass at all.
- **Owned by the Device, and released first in its teardown.** A world's meshes are allocations from an allocator that is about to stop existing, so the ordering has to be right on a path that runs every time the app is backgrounded. Owned anywhere else, that would be a rule somebody has to remember; owned here it is simply what the destructor does. ```Device.h``` forward-declares it so the layering stays one way.
- The practical consequence is that there is no "load the assets once at startup". Whatever loads them lives in ```CreateWorld```, and ```CreateWorld``` runs again on every return from background.
- Two virtuals, not one. ```CreateRenderPasses``` is HOW this world draws and needs a swapchain to exist; ```CreateWorld``` is WHAT it draws. They fail for different reasons (a broken pipeline is a shader problem, a missing model is an asset problem), and a single Initialize would report both as "world failed".
- Assets are owned by the BASE, unlike the TypeScript version where each concrete world keeps its own mesh list and destroys it in an override. Every new world there can forget; here there is nothing to forget.
- The material registry is a member, not the global map the TypeScript version uses. That global has to be emptied on every world change or the next world inherits orphans, and forgetting leaks the old one's buffers. Scoped to the world, the problem does not exist. ```AddMaterial``` refuses a duplicate name rather than replacing, because a silent replace destroys a material that live renderables still point at.
- Member declaration order carries real weight: ```root_``` is declared LAST so it is destroyed FIRST, and the scene is gone before the meshes and materials its renderables pointed at.
- ```Update``` owns the traversal rather than delegating to ```Node```, because of what will share it: each node's behaviours run BEFORE that node's matrix closes, so a behaviour moving its own node or a descendant takes effect this frame, while touching an ancestor whose matrix already closed shows up next frame. That ordering is a design decision and belongs to whoever owns the frame. It is why ```Node``` exposes the single-node step and not only the recursive one.
- ```DestroyNode``` defers to the end of ```Update```, because the traversal is walking the very lists it would erase from. The flush drops any node already covered by a queued ancestor: destroying the ancestor frees the subtree, so the descendant's entry would be a pointer to freed memory by the time its turn came. JavaScript gets away with queueing both; this does not.
- ```FrameContext``` carries the swapchain rather than four loose fields (format, extent, preTransform, image) so they cannot be mismatched. A world that keeps render targets of its own notices the extent changed and rebuilds them itself, since there is no resize callback.
- The frame loop's ```UNDEFINED``` old-layout barrier is only correct because whatever draws covers every pixel: the placeholder is a full screen clear, a world's final pass is a full screen quad. Compositing onto the previous frame would need that to become ```PRESENT_SRC_KHR``` and would drag the swapchain's image count into the world's reasoning.
- The delta handed to ```Update``` is clamped to 100ms. Returning from background produces a gap of seconds, and everything that consumes a delta integrates it. Unclamped, the first frame back teleports the scene.

---

### OpenChiselWorld (cpp, putorana::worlds::openChisel::OpenChiselWorld)
> Called ```graphics::HelloWorld``` until the worlds moved under ```putorana::worlds```. Most of what follows describes the cube it was built around, which was removed with the space carving work; see the class comment for what it does now. The name went to ```worlds::hello::HelloWorld```, which is a hello world again.

- The first world: the cube from ```unitary_cube.glb``` under ROOT, spinning, and a second node carrying the camera, over the live camera feed when there is an AR session.
- It exists to exercise the whole chain at once (asset read, glTF parse, mesh upload, node graph, material, pipeline cache, instanced draw, camera upload, composite) with the smallest content that can show any of it is wrong. A cube is unforgiving on purpose: wrong winding hides the faces you should see and shows the ones you should not, wrong depth order draws the far faces over the near ones, and wrong normals flatten it into a hexagon. Over a camera feed it also stops being a picture and starts being a measurement: geometry that slides when the phone moves is a projection or a pose bug, and nothing else looks like that.
- **Which KIND of camera is the one decision this world makes about AR.** With a session, ARCore owns both the projection and the pose and the node's transform becomes an OUTPUT. Without one (no ARCore, no permission, an unsupported device) it is a ```PerspectiveCamera``` at (2,3,5) aimed at the origin, exactly as it was before, so the cube is still visible on a device that cannot do AR at all.
- In the AR branch the cube is moved to (0,0,-2). ARCore's world origin is wherever the device was when the session started, so a cube left at the origin is a cube the camera is standing INSIDE, clipped away by the near plane, and indistinguishable from a projection that does not work. Two metres down -Z puts it in front of where the user was looking, at the size a one-metre cube should be. Units are metres, and really are.
- The camera feed's failure is NOT the world's. It is logged and swallowed in ```CreateRenderPasses```, because no camera means no background and a cube on a blue field is still a perfectly good picture, the same state as the first frames after every resume, while the camera stack comes up.
- ```Render``` uploads the feed first, outside any rendering scope, then runs the mesh pass with a transparent clear only when the feed actually has content, then the final pass. Both the upload and ```ArCamera::SetFrame``` hold the subsystem's lock through a ```FrameGuard```, because Pause arrives on the UI thread and can free the ArImage mid-copy.
- The camera is a SIBLING of the cube, not a child, so spinning the cube does not spin the camera with it.
- ```LookAt``` in ```CreateWorld``` works because it uses ```ComputeWorldMatrix```, which walks the parent chain rather than trusting the cache, and the cache is empty there, since no frame has run.
- The spin accumulates degrees and never wraps, which is exactly what the unbounded Euler interface is for: the angle climbs past 360 and the quaternion it drives stays well behaved.
- ```Update``` calls the base LAST, and takes the AR frame FIRST. The base is what closes every world matrix, so anything moved after it would land a frame late; the AR pose is what everything else this frame is measured against. This is the seam behaviours will replace.
- ```putorana::worlds::WorldRegistry``` is the only place that names a concrete world; the frame loop, the Device and the JNI layer all know only the World interface. It builds on the first frame with a swapchain, not in ```OnSurfaceCreated```, because pipelines are built for the surface's colour format, and it latches a flag so a world that fails to build is not retried sixty times a second.
- Switching worlds destroys the old one BEFORE building the new one. Two worlds' render targets and chunk meshes alive at once is a real peak on a device this reconstruction has already taken out of memory twice, and keeping the old one as a fallback would be a fiction: its targets were built for this swapchain and nothing puts it back. The cost is that a world which fails to build leaves the placeholder clear rather than the previous scene, which is the more honest picture anyway.
- The REQUESTED world outlives the Device; only the "already built it" latch is cleared when the surface goes. So an app backgrounded on OpenChisel comes back on OpenChisel, and Kotlin never re-sends anything.

---

## Drawing
*What turns the scene into commands.*

### Material (cpp, putorana::graphics::Material)
- A shader plus its render state meeting a set of parameters, in two halves with two lifetimes. The TYPE (the subclass) decides the PIPELINE: shader, blend, cull, depth. The INSTANCE decides SET 2: its uniform buffer, its textures, its descriptor set. Two instances of one subclass differ there and nowhere else.
- ```CreatePipeline``` is ```const``` so the compiler enforces that rule: the result is shared between instances, so anything that changes render state has to be a different subclass.
- **The material builds the pipeline; the render pass owns it.** The WebGPU original caches pipelines in a ```static``` member of each material class, with a comment admitting the cache assumes one device for the life of the app. On Android that is false (the ```VkDevice``` dies every time the app is backgrounded), so a static cache would hand out pipelines belonging to a device that no longer exists, and the crash would land somewhere else entirely. The pass has the right lifetime, owns the attachment formats the pipeline needs, and makes a future shadow pass a second cache rather than a redesign.
- ```MaterialPipeline``` carries the pipeline AND its layout, because ```vkCmdBindDescriptorSets``` needs the layout and Vulkan, unlike WebGPU, does not let the pipeline supply it at the binding call.
- What an implementation owes: chain ```VkPipelineRenderingCreateInfo``` (there is no ```VkRenderPass``` to be compatible with), list the three set layouts in order, declare VIEWPORT and SCISSOR dynamic, and take its vertex input from ```VertexInputFor(format)``` so one material serves both a static and a skinned mesh.
- The set-2 layout is per INSTANCE rather than per class. Two identically defined ```VkDescriptorSetLayout``` objects are compatible in Vulkan, so a pipeline built with one binds a set allocated from the other, which buys away the whole static-lifetime problem for the cost of a duplicated handle.

---

### FlatColorMaterial (cpp, putorana::graphics::FlatColorMaterial)
- The first concrete material and the worked example of the Material contract: what to chain into ```pNext```, what to declare dynamic, where the set layouts come from. The next material is this file with a different shader and a different parameter block.
- Not quite the unlit material the WebGPU renderer starts with. ```mesh_flat.frag``` adds one hardcoded light direction, four lines, because a rotating cube in flat colour is a hexagon and the job of the first thing that draws is to show that winding, normals and depth order are arriving correctly.
- ```frontFace = COUNTER_CLOCKWISE``` has to agree with the Y flip in ```Camera::ProjectionMatrix```: flipping Y reverses winding, so those are one decision made in two files.
- Its parameter buffer is single-buffered, unlike the pass's per-frame data. A parameter written once at load and left alone does not need a copy per frame in flight, and paying for it would mean a descriptor set per frame for every material in the scene.
- The shader modules are locals in ```CreatePipeline```: whichever of its half-dozen early returns fires, they are destroyed, and the pipeline has already kept what it needs from them.

---

### MeshPass (cpp, putorana::graphics::MeshPass)
- The opaque geometry pass. Not a ```VkRenderPass```: dynamic rendering means no such object exists in this project; "render pass" is the logical unit: targets, descriptor layouts, and an algorithm for turning a tree of nodes into as few draw calls as possible.
- Four steps per frame. COLLECT walks the tree keeping renderables whose ```passMask``` has this pass's bit. SORT orders by pipeline, then material, then mesh. UPLOAD writes every object's matrices into set 1 in sorted order. DRAW walks the sorted list in runs sharing all three keys and emits one instanced draw per run.
- **The instancing identity**: slots in the object buffer are filled in the same order the draws are issued, and a draw's ```firstInstance``` is the index of its first slot. Vulkan defines ```gl_InstanceIndex``` as firstInstance plus the instance counter, so it lands exactly on the right slot. That is the whole scheme.
- Sorting is safe only because this pass is entirely opaque and the depth test settles visibility. In a pass with blending the order would be correctness, not a speed-up.
- Ordered by POINTER. The WebGPU version assigns first-appearance ids because JavaScript has no ordering on object references; ```std::less``` gives a total order over unrelated pointers, so the ids are unnecessary.
- Set 1 is a STORAGE buffer and not a uniform one: it is indexed at runtime and unbounded, and a uniform buffer caps at 16KiB on plenty of Android hardware, which is 128 objects.
- Sets 0 and 1 are rebound on every PIPELINE change rather than once before the loop. Binding once would only be legal while every material's pipeline layout stays compatible for those sets, true today, and the kind of invariant that breaks silently the day a material adds a push constant. The cost is one call per distinct pipeline, a handful per frame after the sort.
- Set 2 is rebound after a pipeline change even when the material did not change, because binding a layout disturbs the higher-numbered sets.
- Set numbering is by UPDATE FREQUENCY, which is the only thing it is good for: frame, then object, then material. Vulkan disturbs the sets numbered ABOVE one whose layout changed, so the most volatile goes last and a material switch cannot invalidate the camera.
- ```transparentClear``` clears the colour target to ZERO ALPHA instead of an opaque colour, which is the whole handshake with the camera background: alpha is then 0 wherever no geometry was drawn, and the final pass's mix shows the feed there. Pass false, the default, when there is no feed, and the opaque clear carries straight through that same mix unchanged. No branch anywhere, and one code path whether or not there is AR.
- One colour target, not one per frame in flight, and the barrier is what makes that safe. A pipeline barrier's first synchronization scope covers everything earlier in SUBMISSION order, and submission order spans queue submits, so naming ```FRAGMENT_SHADER``` as the source stage is what stops this frame from overwriting an image the previous frame's final pass is still sampling.
- Depth is cleared and stored ```DONT_CARE```. On a tiler that is the difference between writing a full screen of depth out to RAM and never leaving tile memory with it.
- Growing the object buffer and rebuilding the targets both ```vkDeviceWaitIdle``` first. Rewriting a descriptor set the GPU may be reading is undefined; both events are rare (a doubling, a resize) so the blunt wait beats tracking which sets are live.
- Adding lights changes set 0's layout, and set 0's layout is in every material's pipeline layout, so it rebuilds every pipeline at once. Worth knowing before it looks like a small edit.
- Frustum culling belongs in COLLECT, before an object costs a pipeline lookup and a buffer slot. Not there yet; it is one line when there is a frustum extractor.

---

### FinalPass (cpp, putorana::graphics::FinalPass)
- Puts what the mesh pass drew onto the presented image with one screen-covering triangle that samples it, and, when there is one, puts the camera feed underneath it in the same draw.
- The mesh pass could draw straight into the swapchain image and save a full screen write plus a full screen read, which on a phone is real bandwidth. This pass is not free. It is here because everything that has to happen to a FINISHED image happens in it: compositing the virtual world onto the real one, tone mapping the day the mesh pass draws into a float target, and post effects after that. A geometry pass cannot do those to itself, because they need the whole image while it is still being written.
- **The camera feed composites HERE** because this pass was already going to read the mesh pass's attachment, so the background costs one extra texture fetch and nothing else. The alternative, a camera pass drawing into that attachment before the mesh pass, forces it to be stored and loaded back between the two scopes: a full screen read plus an extra full screen write, around 16MB a frame at 1080p, for a result draw order gives away.
- The compositing is a mix by the mesh target's alpha, in the shader, and not fixed-function blending. Doing it explicitly keeps the colour space explicit, which matters when one input is linear (an ```_SRGB``` texture, decoded on sample) and the other is gamma-encoded camera data.
- Its own set 0: three combined image samplers, no camera, no objects, no material. A pass defines its own numbering; the three-set contract in Material.h is about shaders that draw geometry.
- **A 1x1 placeholder image is bound where the feed is missing.** A descriptor must name a valid view even on frames where nothing samples it in earnest: before the first camera image, and forever on a device with no ARCore. Binding a stand-in is cheaper than a second pipeline, and cheaper than a per-pixel branch to skip a fetch that is already multiplied by zero.
- The push constant block is declared ONCE, visible to both stages, rather than split into a vertex range and a fragment range. Ranges that overlap in memory but differ in stage flags are legal and are a reliable way to spend an afternoon on a validation error.
- ```loadOp``` is ```DONT_CARE```, not ```CLEAR```: the triangle covers every pixel, so clearing would write a full screen of colour that is immediately overwritten.
- The source sampler is ```NEAREST``` because that is a 1:1 copy: source and target are both the swapchain extent, so every sample lands exactly on a texel and linear filtering would be four taps computing the same value. The FEED's sampler is ```LINEAR```, because that one is a genuine rescale from the sensor's resolution and it upsamples the chroma plane on the way.
- The descriptors are only rewritten when a view actually changes (a resize, or the first camera image), and it waits for the GPU first: rewriting a set a frame in flight is reading is undefined.

---

### Frame (cpp, putorana::graphics::DrawFrame)
- Free functions, no class. Drawing a frame is the renderer's job, and the renderer is this namespace rather than an object. Wrapping it in a ```class Renderer``` would only be a namespace with worse ergonomics.
- Runs once per vsync on the render thread. ```frameTimeNanos``` is when the frame is meant to be displayed, not when the call began, so animation driven from it stays smooth when a frame lands late.
- Swapchain recreation has two triggers and both matter. ```surfaceChanged``` is the obvious one. The other is ```VK_ERROR_OUT_OF_DATE_KHR``` or ```VK_SUBOPTIMAL_KHR``` coming back from acquire or present, and on Android that is the trigger that fires on rotation, because the compositor can change the surface transform without the window size moving at all.
- Those two results need opposite handling. ```OUT_OF_DATE``` from acquire leaves the semaphore unsignalled, so the frame can be dropped on the spot. ```SUBOPTIMAL``` signals it, so the frame has to be finished normally or that semaphore stays signalled with nobody waiting on it.
- ```vkQueueSubmit2``` signals the binary present semaphore and the timeline value from one array. Before synchronization2 the timeline values lived in a parallel struct whose arrays had to be kept index aligned by hand.
- Dynamic rendering, so there is no ```VkRenderPass``` and no ```VkFramebuffer``` anywhere in the project.
- Drives the world when there is one: ```Update``` before the acquire (it touches no Vulkan, only nodes and matrices, so it has nothing to wait for), then ```Render``` between the two layout barriers. With no world installed it falls back to the slow colour sweep it always had, which is what proves acquire/submit/present work before there is anything to draw.
- **The AR session's tick happens first of all, before ```BeginFrame``` and before the acquire.** It blocks, for up to ARCore's built-in 66ms, waiting for the next camera image, so anywhere later it would sleep while holding a swapchain image and a frame slot. It is also the thing everything else this frame is about: a tick buried inside a render pass would leave whichever pass ran first working from the previous frame's answer, which in AR is visible as geometry sliding against the world.
- ```GpuProfiler::BeginFrame``` is the first thing recorded into the command buffer, and has to be: it records the query pool reset, which must precede every timestamp written into that pool. The outermost ```GpuScope``` is named ```frame```, so the overlay shows what the passes inside it do NOT account for: barriers, transitions, and whatever the driver does between them.
- This file is where the two lifetimes meet, and it holds no AR state to make that safe: it asks ```subsystem_holder``` every frame and does nothing when the answer is null.

---

## Instrumentation
*Measuring what the GPU actually did. Debug facilities, and they die with the device like everything else.*

### GpuProfiler / GpuScope (cpp, putorana::graphics::GpuProfiler)
- GPU timing for named scopes, by timestamp query. Owned by the Device, so its query pools go away and come back on every trip through Home, and the Kotlin side has to tolerate that.
- **A clock read around the calls would measure nothing.** Recording a command buffer takes almost no time and says nothing about the work: ```vkCmdDraw``` returns long before the GPU has drawn anything, so a CPU timer around a render pass measures how fast the driver builds a command list. The only way to learn what the GPU spent is to have the GPU write the time itself.
- **The numbers are always a frame or two old, on purpose.** A timestamp is readable only once the submission carrying it has retired. This keeps one pool per frame in flight and reads a slot's results at the start of the NEXT frame that reuses it, by which point ```FrameRing::BeginFrame``` has already waited on the timeline for exactly that, so the read never blocks. Waiting on the frame that just ended would stall the CPU on the GPU every frame in order to measure the GPU, which is a fine way to make the numbers wrong in the direction of "everything is fast".
- Scopes NEST: ```Begin```/```End``` form a stack, so one scope around the whole frame contains one per pass. Timestamps are written with ```ALL_COMMANDS``` at both ends, the conservative reading, and it means two adjacent scopes cannot appear to overlap.
- ```GpuScope``` is the RAII form and the only one worth using: a scope has to be closed on every path out of a render function, and the paths out of a render function are early returns.
- A device whose queue family reports no timestamp support is NOT a failure. The profiler is created DISABLED and every call becomes a no-op, because refusing to start the renderer there would trade a working app for a debug overlay. ```FrameContext::profiler``` is a raw pointer for the same reason: it may legitimately be absent, and ```GpuScope``` then does nothing rather than crashing.
- ```Snapshot``` takes a lock, the only thing in the renderer that does, because it is read from the UI thread while the render thread writes the next frame's numbers. The values are exponentially smoothed, and that is not cosmetic: raw per-frame GPU times jump by tens of percent frame to frame, and unsmoothed the overlay is unreadable.
- The ticks are masked to ```timestampValidBits``` and scaled by ```timestampPeriod```. Bits above the valid count are garbage, not zero.

---

## Content pipeline
*Getting art and shaders from a folder on disk into the running app.*

### Assets (cpp, putorana::assets)
- Reading files out of the APK. An app's assets are not files on a filesystem: they live inside the APK, usually compressed, and the only way at them is ```AAssetManager```. There is no path ```fopen``` would take, which is why every loader here consumes bytes rather than a filename.
- The Java ```AssetManager``` arrives through a JNI call of its own rather than through ```JNI_OnLoad```, which has no Context to get one from. ```VulkanSurfaceView```'s init makes the call, before the surface callback is registered, so it cannot lose a race with a surface that already exists.
- Native keeps a GLOBAL JNI reference to the Java object, and this is not optional: ```AAssetManager_fromJava``` returns a pointer whose validity is tied to that object staying alive, and a local reference dies when the JNI call returns. Skipping it is the classic form of this bug: it works until the collector runs.
- The application context's AssetManager, not the view's: native holds it for the life of the process, and an Activity-scoped one would be pinned across configuration changes.
- Authoring files live in ```assets/``` at the repo root (.blend and the .glb next to it); only the .glb is copied into ```app/src/main/assets/```, which is what Gradle packages. Pointing Gradle at the root folder instead would put the .blend sources in the APK.

---

### GltfLoader (cpp, putorana::graphics::LoadGltf)
- Reads a .glb from the assets and turns its scene into our node graph. An assimp node becomes a Node with its transform DECOMPOSED back into position/rotation/scale, not stored as a matrix, because a Node's transform IS those three and everything downstream edits them. ```aiMatrix4x4::Decompose``` reads assimp's row-major matrix correctly on its own; transposing first would silently produce a transform that is almost right.
- A node carrying several meshes (glTF calls them primitives, and they exist so each can have its own material) becomes the node plus one child per extra, named ```<node>_prim1```. A Renderable is one per Node, and the children inherit the parent's transform by sitting under it, which is exactly right: they are the same object cut up by material.
- A mesh referenced by several nodes is uploaded ONCE and pointed at by several Renderables. That is the cache, and it is what makes two hundred wall tiles cost one upload.
- Roots come back DETACHED. The caller decides whether they go under ROOT or are kept aside as a prefab template; returning them parented would take that choice away.
- Parsed from MEMORY, not from a path: an asset in an APK is not a file, so assimp's default IO system has nothing to open. The ```"glb"``` hint picks the importer without a filename to guess from, and it is also why only .glb works, since a plain .gltf would send the parser looking for sibling .bin and image files that do not exist inside an APK.
- assimp wraps the file in a root of its own, so that wrapper is dropped and the file's real top-level nodes are used, UNLESS the root carries geometry itself, in which case it is a real node and unwrapping would throw the model away.
- Post-processing, and what is deliberately absent. NOT ```PreTransformVertices```, which flattens the hierarchy into one baked mesh, the exact opposite of wanting a node graph. NOT ```FlipUVs```: glTF puts the UV origin top-left and so does Vulkan; the flag exists for OpenGL's bottom-left and would turn every texture upside down. NOT ```FlipWindingOrder``` or ```MakeLeftHanded```: glTF is right-handed with CCW front faces, which is what the pipelines are built for.
- The smoothing angle is set to 66 degrees. It only applies to a mesh that arrived with no normals, but assimp's default of 175 smooths practically everything, and a cube whose eight corners have been smoothed together reads as a lighting bug rather than a missing attribute.
- Not read in this pass: materials (renderables come back with a null material), skins (a mesh with bones loads as static in its bind pose and logs it: its bone indices address a joint array only the skin defines, so extracting them early would mean inventing that ordering twice), animations, and the file's cameras and lights.

---

### Shaders (glsl in assets/shaders, tools/compile_shaders.py, cpp ShaderModule)
- Wired into Gradle: ```compileShaders``` runs before ```preBuild```, so a build cannot package a stale .spv. A GLSL error fails the build with glslc's own file:line message, verified by breaking a shader on purpose. Gradle's up-to-date checking (```inputs.dir``` / ```outputs.dir```) keeps it quiet on a build that changed nothing.
- **No shader compilation in the app.** Every module is built ahead of time and loaded from the APK as finished SPIR-V. The one failure mode of that arrangement is a shader that was edited and not recompiled, which then ships as the previous version, so the script exits non-zero on any failure, and tracks ```#include``` dependencies through glslc's ```-MD``` so editing an included file rebuilds what includes it.
- Sources live in ```assets/shaders``` next to the .blend files (the authoring folder); ```tools/compile_shaders.py``` compiles them into ```app/src/main/assets/shaders```, which is what Gradle packages. The output is gitignored: it is derived, and a fresh clone runs the script before its first build.
- Named ```<name>.<stage>.spv```, keeping the stage in the filename, so a material's vertex and fragment shaders cannot collide and the runtime path reads as what it is.
- Built with ```-g``` and ```-O0```, on purpose and together. ```-g``` embeds the GLSL source (```OpSource```), the line mapping (```OpLine```) and the original identifiers (```OpName```), which is what makes RenderDoc show the actual file instead of decompiled SPIR-V, and what lets its debugger step through it. Optimisation would fold and rename exactly what ```-g``` just recorded, so the two flags are not independent choices. ```--release``` flips both.
- ```--target-env=vulkan1.3``` matters: without it glslc targets SPIR-V 1.0 and rejects later constructs while blaming the shader rather than the target.
- **glslc comes from the NDK version app/build.gradle.kts pins**, ahead of ```ANDROID_NDK_HOME```. That is not the obvious order and it is deliberate: ```ANDROID_NDK_HOME``` is a machine-wide setting that drifts, and on this project's own machine it pointed at an NDK three major versions behind the pinned one. Shaders built by a compiler years apart from the toolchain that builds the renderer is a difference nobody goes looking for. An explicit ```GLSLC``` still wins over everything.
- ```ShaderModule``` is move-only RAII rather than a load/destroy pair, because a material's ```CreatePipeline``` loads two modules and then has half a dozen early returns before ```vkCreateGraphicsPipelines```, and every one of them would leak. It is short-lived by design: a module is only needed while a pipeline is being built.
- ```fullscreen.vert``` generates ONE oversized triangle from ```gl_VertexIndex```, not a quad, and not for elegance. A quad is two triangles meeting along the diagonal, and the GPU shades in 2x2 pixel quads, so every quad straddling that seam is shaded twice. A single clipped triangle has no seam. Its screen UVs are derived from the index; its CAMERA UVs cannot be, and arrive as push constants: the sensor has a fixed orientation and aspect ratio, neither of which is the viewport's, and ARCore owns the mapping between them.
- ```composite.frag``` does two jobs in that one triangle, because both need the mesh pass's attachment and splitting them would mean storing it and loading it back: YUV_420_888 to RGB, then the mix of the mesh output over it by alpha.
- **BT.601, studio swing.** Luma occupies 16..235 and chroma 16..240, which is what a camera pipeline produces; treating it as full range is the classic mistake and shows up as milky blacks and clipped highlights.
- What comes out of that matrix is GAMMA-ENCODED (display-ready values, the same encoding a JPEG carries) while the mix happens in linear light and an ```_SRGB``` swapchain re-encodes on write. So it is linearised first, with the exact sRGB EOTF rather than ```pow(2.2)```: the piecewise linear segment near black is the part the approximation gets wrong, and dark is where a camera feed spends much of its range indoors. Mixing gamma-encoded values against linear ones looks like a wrong blend factor.
- Sampling an ```_SRGB``` image decodes to linear in hardware, and alpha is never sRGB-encoded by spec, which is what makes the mesh target's alpha usable directly as the mix factor.
- Tone mapping, when it comes, goes ABOVE the mix and on the mesh contribution ALONE: that half is scene-referred, the camera feed is already display-referred and must not be touched.

---

## The Android side
*Kotlin. Owns the window, the thread and the activity lifecycle; owns no renderer state.*

### Manifest (app/src/main/AndroidManifest.xml)
- **AR Required, not optional**, in two places that have to agree: ```uses-feature android.hardware.camera.ar required="true"``` and the ```com.google.ar.core``` meta-data. There is no mode of operation without tracking, the same way there is no fallback without Vulkan 1.3. Both consequences are wanted here: the Play Store hides the app from devices with no ARCore support, and installs ARCore alongside it. An AR Optional app would declare ```required="false"``` in both and check ```ArCoreApk_checkAvailability``` before touching a session.
- ```CAMERA``` is a runtime permission and ARCore never prompts for it: it just fails. Asking is the activity's job, and it has to be settled before the session is created.
- **```HIGH_SAMPLING_RATE_SENSORS``` is the non-obvious one.** ARCore's tracking is visual-inertial: it wants the IMU at 200Hz with a 5ms report delay, and since API 31 sampling motion sensors that fast requires this permission. Without it the sensor registration fails and ```ArSession_resume``` returns ```AR_ERROR_FATAL``` with "Failed to register sensor to queue 0" in logcat, nothing that mentions permissions, and nothing wrong with the camera. It is a normal protection level, so it is granted at install and never requested. The SDK's own samples do not declare it because they do not target 31+.
- ```screenOrientation="portrait"```. A rotation here is not a relayout: it destroys the Activity, the Surface, the VkDevice, the swapchain and the whole world and rebuilds all of it, with the AR session the only survivor. Not worth paying in an app whose interface is the camera.
- That lock does NOT make the renderer's rotation handling redundant, which is why it stays: the swapchain's ```preTransform``` can be non-identity even with the orientation fixed, and since API 36 Android ignores the restriction on large screens: on a tablet the app rotates anyway. ```ArCamera``` and ```CameraFeed``` keep composing ```SurfaceRotation```, and that is what keeps it correct there.

---

### MainActivity (java, dev.dongeronimo.arreconstructor::MainActivity)
- Owns the AR session's lifecycle and nothing else about AR. ```NativeAr.create``` once the permission is settled, ```resume``` from onResume, ```pause``` from onPause, before ```super```, so the camera is released on the way out rather than after the framework has begun tearing the window down.
- **The permission dialog breaks the usual ordering**, which is what ```activityResumed``` exists for: the grant can land long after onResume has run, so the code that starts the camera cannot assume it is the one being resumed. Without that flag, granting the permission on first launch produces a session nobody ever resumes.
- A denied permission is not a crash. The overlay says so and the renderer keeps drawing the cube on its clear colour, which is a good deal more informative than a black screen.
- The GPU timings are POLLED, four times a second, and not pushed. A callback per frame would cross the JNI boundary thirty times a second to update a label nobody can read that fast, and would have to hop to the UI thread each time. Four times a second is faster than the smoothing settles anyway. The polling stops in onPause BEFORE anything else, because the device the numbers come from is about to be torn down.
- ```goFullscreen``` does two separate things and doing only one is the usual mistake. ```setDecorFitsSystemWindows(false)``` stops the decor view shrinking the content to fit the bars. Without it the SurfaceView is built for a shorter rectangle than the screen, and the swapchain follows. Hiding the bars is what gets them off the pixels. Sticky immersive, so an edge swipe brings them back as a transient overlay without resizing the window, because a resize means a ```surfaceChanged``` and a swapchain rebuild every time. Re-asserted on every focus gain, since the bars come back on their own after an app switch or a dialog.
- The overlays are declared AFTER the surface view in the layout, and that is the z order: the SurfaceView composites behind the window, so last means on top. No ```setZOrderOnTop```, no extra window: those exist for putting a SurfaceView ABOVE other views, which is the opposite problem.

---

### NativeAr (java, dev.dongeronimo.arreconstructor::NativeAr)
- The Kotlin side of ```putorana::ar::Subsystem```: ARCore, and nothing to do with Vulkan. Unlike ```NativeRenderer```, these are called from the UI THREAD, because the session's life follows the activity and not the surface. That distinction is the whole reason it is a separate object, and the native side takes a lock precisely because of it.
- It remembers the last display geometry and replays it after ```create```, and that is not belt-and-braces. **The two things that determine the geometry arrive in an order nobody controls**: with the permission already granted the session exists before the surface, but with the dialog shown the grant lands long after the first ```surfaceChanged```, so the geometry is reported to a session that does not exist yet and is silently dropped, and nothing would ever report it again, because ```surfaceChanged``` does not fire twice for the same surface. The symptom is a camera image rotated ninety degrees, but only on the very first run after an install.
- Failure is reported, not thrown. No session means no camera background, and the cube still draws.

---

### NativeProfiler / PassTiming (java, dev.dongeronimo.arreconstructor::NativeProfiler)
- Reads the renderer's GPU timings, and it is the ONE native call that does not come from the render thread. Everything on ```NativeRenderer``` must, and the native side relies on that to take no locks at all; ```GpuProfiler::Snapshot``` takes one so that this can be an exception.
- The entries are whatever the passes declared, in the order they were recorded, so an enclosing scope comes before the passes inside it. **This side never has to know what passes exist**, and adding one changes no Kotlin at all.
- An empty array is normal and not an error: before the first frames retire, while there is no surface, and forever on a device with no timestamp support.
- The JNI side deletes both local references per entry. The local reference table is small and this runs several times a second; two leaked objects per entry would overflow it long before the frame loop noticed.

---

### NativeRenderer (java, dev.dongeronimo.arreconstructor::NativeRenderer)
- Mirrors the c++ public interface.
- Loads the library (triggering JN_OnLoad).
- Holds no state and does no threading of its own. Every method except ```setAssetManager``` must be called from the render thread, and the native side relies on that and takes no locks.
- ```setAssetManager``` is the exception on both counts: it only stores a pointer, and it runs before the render thread exists. It has to be called before the first ```surfaceCreated```, because the world is built from assets as soon as there is a device to build it on.

---

### VulkanSurfaceView (java, dev.dongeronimo.arreconstructor::VulkanSurfaceView)
- The window Vulkan draws into. A SurfaceView and not a TextureView: it gets its own compositor layer, whose ANativeWindow is exactly what ```vkCreateAndroidSurfaceKHR``` wants. A TextureView's buffers detour through the view hierarchy's own rendering, which costs a copy per frame and adds latency.
- Holds nothing but the render thread, and only while there is a surface to feed it. Surface, device and swapchain all have lifetimes tied to the surface rather than to the view or the Activity, so keeping any of them here would tie them to the wrong thing.
- Its ```init``` hands the native side the AssetManager, before registering the surface callback so it cannot lose a race with a surface that already exists. The APPLICATION context's AssetManager, not the view's: native holds a global reference for the life of the process, and an Activity-scoped one would be pinned across configuration changes.
- ```surfaceChanged``` tells ARCore the display geometry BEFORE it tells the render thread, and from THIS thread rather than the render thread. It needs the display rotation, which is a property of the view hierarchy, and it is the one piece of per-surface information a session whose life is the activity's still has to be told. Every rotation comes through here, which is what keeps the camera background upright with nothing being rebuilt: only the UVs ARCore hands back change.
- ```display``` is a platform type and null for a detached view. A surface that is changing size is attached by definition, but the ```ROTATION_0``` fallback costs nothing and beats an NPE on a path this hard to reproduce.

---

### RenderThread (java, dev.dongeronimo.arreconstructor::RenderThread)
- Drives the c++ side of the renderer, using NativeRenderer.
- Owns one thread, all native calls go thru it, so we can skip locking in cpp
- Thread life = surface life
- Comes from the Choreographer, dont use a while(true).

---

