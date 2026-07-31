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
  - Allocator (VMA): created with the Device, destroyed with it
  - World (scene tree, assets, render passes): owned by the Device, released FIRST in its teardown
  - Buffers, meshes, anything allocated: nested inside the Allocator's life, so they die every time the app goes to background. Nothing may be loaded once and cached across surfaces.

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

## Allocator (cpp, putorana::graphics::Allocator)
- The one ```VmaAllocator```. Every ```VkBuffer``` and ```VkImage``` is carved out of it, and it is owned by Device — so it dies and is rebuilt on every trip through Home, and so does everything allocated from it.
- Exists because ```maxMemoryAllocationCount``` is a real limit, routinely 4096 on Android. One ```vkAllocateMemory``` per mesh burns through it. VMA takes big blocks and suballocates.
- Must be created after ```volkLoadDevice```. VMA is built with ```VMA_DYNAMIC_VULKAN_FUNCTIONS```, so the only two entry points it is handed are ```vkGetInstanceProcAddr``` and ```vkGetDeviceProcAddr``` and it fetches the rest through them. Forgetting to fill those two in is the classic volk+VMA crash: a table of nulls, called on the first allocation.
- ```unifiedMemory()``` checks for a memory type that is ```DEVICE_LOCAL``` and ```HOST_VISIBLE``` at once. Every Android GPU has one, because there is a single pool of RAM. It is queried instead of assumed so the day it is false is a line in logcat rather than mysteriously slow uploads.
- ```vmaDestroyAllocator``` asserts in debug when allocations are still alive. That is the leak check: it fires precisely when something outlived the Device it came from.

---

## Buffer (cpp, putorana::graphics::Buffer)
- A ```VkBuffer``` plus its VMA allocation, permanently mapped, written straight from the CPU.
- No staging buffer anywhere, and that is the platform rather than a shortcut. Staging exists because a discrete GPU has memory the CPU cannot reach. Here there is one pool of RAM and ```DEVICE_LOCAL|HOST_VISIBLE``` memory types exist, so a staging copy would be memcpy'ing RAM to RAM. No buffer here carries ```TRANSFER_DST```.
- What that memory usually *is*, though, is write-combined and uncached: writes are fast, reads are tens of times slower because every one goes to DRAM. So the mapped pointer is write-only, written forward, never used as scratch and never read back — not even for a read-modify-write. ```VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT``` states that contract to VMA and is what gets the fast memory type back.
- ```Flush``` is called on every write. VMA turns it into nothing when the memory is ```HOST_COHERENT```, which on Android it nearly always is — it is unconditional so the one device where it is not behaves identically.
- ```Write``` is bounds-checked and refuses rather than corrupting whatever VMA suballocated next door.

---

## Mesh (cpp, putorana::graphics::Mesh)
- Indexed geometry on the GPU: one vertex buffer, one index buffer, the counts to draw them with, and a local AABB. Knows nothing about materials, pipelines or the scene — a Mesh is something you bind, not something that draws itself.
- Two independent axes, and keeping them independent is the design. ```VertexFormat``` (Static/Skinned) changes the stride and the vertex input state and nothing else; ```MeshStorage``` (Immutable/Mutable) changes the memory layout and nothing else. Every combination works and neither axis knows the other exists.
- One interleaved vertex buffer. Position is the first member of every format, so bounds, culling and any future position-only pass use one stride and offset 0 regardless of what the mesh is.
- ```StaticVertex``` is 32 bytes, ```SkinnedVertex``` is 52. Weights are ```R32G32B32A32_SFLOAT``` and stay exactly as the exporter produced them — they are normalised floats, not a packed encoding, and nothing happens to them between the buffer and the shader. Joint indices are ```R8G8B8A8_UINT```, which is what glTF itself uses for JOINTS_0 and caps a mesh at 256 joints; the format has mandatory vertex-buffer support, so there is nothing to query at runtime.
- Shader locations are shared by both formats — 0 position, 1 normal, 2 uv, 3 boneIds, 4 weights — so a shader written for Static reads a Skinned mesh correctly and just ignores 3 and 4.
- Indices arrive as uint32 from every caller and are narrowed to uint16 when the vertex *capacity* fits, halving index bandwidth on nearly every real mesh. Capacity and not the current count, so a mutable mesh that grows later does not need a different index type than it was built with.
- ```VertexInput``` owns its arrays on purpose. ```VkPipelineVertexInputStateCreateInfo``` is two pointers and two counts, so building it from temporaries hands ```vkCreateGraphicsPipelines``` dangling memory — which shows up as garbage geometry on one driver and works fine on another.
- Mutable does not cost a staging buffer, it costs memory. Unified memory solves the transfer; it does not solve timing. While frame N records on the CPU, the GPU is still reading what frame N-1 bound, so overwriting is a race with no validation message and a symptom (geometry flickering between two shapes) that looks like a bug in whatever generated the vertices. So a Mutable mesh holds one region per frame in flight inside the same allocation.
- That is why ```Bind```, ```Draw``` and ```Update``` all take a frame index: pass ```FrameSlot::index``` and it is right by construction. An Immutable mesh has one region and ignores the argument, so callers never branch on storage type.
- The contract that follows: whoever writes a Mutable mesh writes the whole region it is about to draw, every frame it changes. Data written into one slot is not in the others. Initial data given to ```Create``` goes into all of them, so a mutable mesh that is never updated still draws what it was born with.
- There is no ```Draw()``` here, on purpose. A draw is not a property of the geometry: ```instanceCount``` and ```firstInstance``` come from a run of objects that share a pipeline, a material and this mesh, and only the pass doing the batching knows about that run. A ```Draw()``` on the Mesh would have to take both as arguments anyway, and would hide ```indexCount``` — the number the batching loop needs — inside itself, quietly inviting one draw call per object.
- ```Bind``` stays, because it is the one thing that genuinely is mesh-private: which region of the buffer this frame's data lives in. The pass calls ```Bind``` then issues its own ```vkCmdDrawIndexed``` with ```mesh.indexCount(frameIndex)```. A count of zero means an unwritten region — skip it.
- ```Aabb``` (local space here, world space once a Renderable has transformed it) lives in this header because geometry is where the vocabulary belongs.

---

## World (cpp, putorana::graphics::World)
- A scene plus the way that scene is drawn. It owns four things and that ownership IS the class: the tree rooted at ROOT, the assets in it (meshes, materials), its own sequence of render passes, and the frame's update.
- The pass chain belongs to the world and not to the frame loop because each world draws differently — one is mesh+final, another cubemap+shadow+gbuffer, a volume renderer is something else again. ```Frame.cpp``` does acquire, layout transitions, submit and present, and knows about no pass at all.
- **Owned by the Device, and released first in its teardown.** A world's meshes are allocations from an allocator that is about to stop existing, so the ordering has to be right on a path that runs every time the app is backgrounded. Owned anywhere else, that would be a rule somebody has to remember; owned here it is simply what the destructor does. ```Device.h``` forward-declares it so the layering stays one way.
- The practical consequence is that there is no "load the assets once at startup". Whatever loads them lives in ```CreateWorld```, and ```CreateWorld``` runs again on every return from background.
- Two virtuals, not one. ```CreateRenderPasses``` is HOW this world draws and needs a swapchain to exist; ```CreateWorld``` is WHAT it draws. They fail for different reasons — a broken pipeline is a shader problem, a missing model is an asset problem — and a single Initialize would report both as "world failed".
- Assets are owned by the BASE, unlike the TypeScript version where each concrete world keeps its own mesh list and destroys it in an override. Every new world there can forget; here there is nothing to forget.
- The material registry is a member, not the global map the TypeScript version uses. That global has to be emptied on every world change or the next world inherits orphans, and forgetting leaks the old one's buffers. Scoped to the world, the problem does not exist. ```AddMaterial``` refuses a duplicate name rather than replacing, because a silent replace destroys a material that live renderables still point at.
- Member declaration order carries real weight: ```root_``` is declared LAST so it is destroyed FIRST, and the scene is gone before the meshes and materials its renderables pointed at.
- ```Update``` owns the traversal rather than delegating to ```Node```, because of what will share it: each node's behaviours run BEFORE that node's matrix closes, so a behaviour moving its own node or a descendant takes effect this frame, while touching an ancestor whose matrix already closed shows up next frame. That ordering is a design decision and belongs to whoever owns the frame. It is why ```Node``` exposes the single-node step and not only the recursive one.
- ```DestroyNode``` defers to the end of ```Update```, because the traversal is walking the very lists it would erase from. The flush drops any node already covered by a queued ancestor — destroying the ancestor frees the subtree, so the descendant's entry would be a pointer to freed memory by the time its turn came. JavaScript gets away with queueing both; this does not.
- ```FrameContext``` carries the swapchain rather than four loose fields (format, extent, preTransform, image) so they cannot be mismatched. A world that keeps render targets of its own notices the extent changed and rebuilds them itself — there is no resize callback.
- The frame loop's ```UNDEFINED``` old-layout barrier is only correct because whatever draws covers every pixel: the placeholder is a full screen clear, a world's final pass is a full screen quad. Compositing onto the previous frame would need that to become ```PRESENT_SRC_KHR``` and would drag the swapchain's image count into the world's reasoning.
- The delta handed to ```Update``` is clamped to 100ms. Returning from background produces a gap of seconds, and everything that consumes a delta integrates it — unclamped, the first frame back teleports the scene.

---

## Node (cpp, putorana::graphics::Node)
- A transform in a hierarchy plus whatever optional components hang off it. A node by itself is a position, a scale and an attitude; what it IS comes from what is attached. Renderable present means it is drawn, camera present means it is a camera, light present means it is a light. A node carrying none is a pure pivot, which is not a degenerate case but the most common one — every parent is one.
- **Rotation is a quaternion inside and unbounded Euler degrees outside.** The quaternion is the source of truth: it feeds the matrices and any future interpolation. The Euler angles are an editable VIEW of it, with Unity's semantics.
- Setting ```eulerAngles``` stores the numbers verbatim, unclamped. Set (500, -720.5, 1234) and read back (500, -720.5, 1234) — that is what lets an animation drive an angle continuously through 360 without the value jumping. Setting the quaternion directly makes those raw numbers meaningless, since a quaternion cannot say whether it got there by turning 30 degrees or 390, so the cache is flagged stale and the next read derives canonical angles instead (x in [-90,90], y and z in [-180,180]).
- The Euler convention is degrees, applied Z then X then Y about WORLD axes, so R = Ry * Rx * Rz. Written out as three explicit ```angleAxis``` products rather than through a library helper, because every library spells the order differently and half of them mean intrinsic axes by it. Verified numerically against Ry*Rx*Rz over 20000 random angles (worst element error 1.2e-15) and for round-trip through the canonical extraction.
- Under gimbal lock the extraction pins z to 0 and gives the whole remaining turn to y, the same choice Unity and three.js make. Reading back (90, 15, 0) after setting (90, 35, 20) is correct: it is the same rotation.
- The Euler cache members are ```mutable``` because reading ```eulerAngles()``` on a stale cache derives and stores. That changes the cache, not the rotation — the node's orientation is identical before and after, which is the case ```mutable``` exists for.
- ```position``` and ```scale``` are public and mutable in place, three.js style, because nothing is derived from them. Rotation is not, because the Euler cache is. That asymmetry is the entire reason one is a field and the other a pair of methods.
- **A node owns its children**, and a root is owned by whoever called ```Create```. The constructor is private so a stack-allocated node cannot be parented by accident.
- The alternative — every node a raw pointer, one flat container owning them all — is a real design and a common one, but it needs that container to exist and there is no World class yet. An owning tree needs nothing but itself and fits this app's lifetime exactly: everything dies together when the surface goes, so releasing the root releases the scene, with no traversal and nothing left to leak. It is also what makes destroying a prefab instance one line instead of a walk that erases nodes from a flat list.
- ```SetParent(newParent)``` is the normal way to reparent. It returns false and changes NOTHING in two cases: a cycle, and a node that is still a root — whose ```unique_ptr``` is held by its creator, so there is nobody for the method to take it from. That one is spelled ```newParent.AddChild(std::move(ptr))```, the only form that says what happens to the caller's pointer.
- The cycle check in ```SetParent``` runs BEFORE the detach, deliberately. Leaving it to ```AddChild``` would mean the subtree is already unhooked by the time the refusal happens, so a failed reparent would tear a branch out of the scene and then delete it.
- ```AddChild``` has its own check for the same reason: a subtree that owns itself would never be destroyed, and the destructor would recurse forever if anything tried.
- ```Detach``` is ```[[nodiscard]]``` because dropping the result destroys the subtree. That is a legitimate way to delete a branch, but it should be written on purpose: ```node->Detach().reset()```.
- Neither copyable nor movable. Children hold a raw back-pointer to their parent, so a move would leave every child pointing at a corpse. Copying a subtree is a different operation with different semantics (shared assets, fresh per-object state) and belongs to whatever implements prefabs.
- ```worldMatrix()``` is the cached read, O(1), valid only after ```UpdateWorldMatrices``` has run over the node this frame. ```ComputeWorldMatrix()``` walks up the chain and is always current at O(depth) — for use outside the frame loop, such as right after building a tree. The update is top-down so each node composes onto its parent's already-refreshed cache, which is what makes the tree O(n) rather than O(n * depth).
- ```LookAt``` aims the node's -Z, the glTF camera convention, which is what makes the view matrix fall straight out of the inverse of the world matrix. Note this is the OPPOSITE of Unity, whose LookAt aims +Z. It substitutes a different up axis when the requested one is parallel to the aim — aiming a camera straight down is not exotic, and the degenerate basis would otherwise produce a NaN quaternion that spreads silently through every matrix the node touches.
- ```CopyLocalFrom``` writes the members directly rather than going through the setters, because either setter would destroy half the state: one flattens the raw angles, the other rebuilds the quaternion from them. Copying all three pieces plus the sync flag reproduces the source exactly, whichever way it was last edited.

---

## Renderable (cpp, putorana::graphics::Renderable)
- What a scene node draws: one Mesh, one Material, and the per-object decisions that belong to the object rather than the asset. Two pointers and two fields, and no Vulkan anywhere in it.
- Owns neither the Mesh nor the Material. Those are assets shared by reference — two hundred wall tiles point at one Mesh and one Material between them — and whatever loaded them destroys them, which means dying with the Device.
- Copyable, and the copy IS the prefab clone: pointers shared, per-object state copied. The TypeScript renderer needs a hand-written ```cloneRenderable``` for this and has a bug waiting in it every time a field is added and the clone is not updated. A ```static_assert``` pins the property so a ```unique_ptr``` member cannot creep in and silently break it.
- ```passMask``` is an OR of ```RenderPassBit```, typed as ```uint32_t``` because the OR of two enumerators is not itself an enumerator. Each pass walks the tree and skips whoever lacks its bit — that is how one object is drawn by the main pass and again by every shadow pass.
- ```RenderPassBit::Skinned``` replaces ```Main``` rather than joining it: the per-object descriptor set is a different shape (a pool of bone matrices, not one model matrix), so a mesh cannot be in both. It is also a separate question from ```VertexFormat::Skinned``` — the format says what the vertices contain, the bit says who draws them, and a skinned mesh can be handed to the main pass and drawn rigid in its bind pose.
- ```castsShadow``` is on the Renderable and not the Material on purpose. Two objects sharing a material are allowed to disagree, and it is the object that occupies space. The case that forces it is ground clutter — geometry thinner than a shadow-map texel contributes no lighting information and a lot of artefact, and costs a draw in every shadow map. Nothing reads it yet.
- ```material``` starts null: the loader builds renderables from a glTF, which carries none of ours. A pass meeting a null material substitutes a loud fallback rather than skipping the draw, because an object screaming "I have no material" debugs faster than an object that is silently absent.
- ```WorldBounds``` takes the matrix as an argument instead of reaching for a node, which is what keeps the scene graph and the renderer separable. All eight corners, because rotation means the extreme corners of the local box are generally not the extreme corners of the transformed one.

---

## Camera (cpp, putorana::graphics::Camera)
- The projection half of a camera, hanging off a Node like a Renderable does. The VIEW is not here: it is the inverse of the owning node's world matrix, because the camera looks down its node's -Z. That is what lets a camera be parented to a ship and follow it with no code.
- Not a 29-line class like the WebGPU original it is ported from, because three transforms sit between a right-handed world and an Android screen and none of them are optional.
- ```perspectiveRH_ZO``` and not ```glm::perspective```. RH is the glTF convention the whole engine uses; ZO is Vulkan's [0,1] clip depth, which is NOT GLM's default — GLM ships OpenGL's [-1,1] and only switches globally with ```GLM_FORCE_DEPTH_ZERO_TO_ONE```. A global that halves everyone's depth precision if someone drops it is worse than naming the variant at the one call site.
- ```projection[1][1] *= -1``` because Vulkan's NDC +Y points DOWN, unlike OpenGL and WebGPU. Skipping it renders the scene upside down and, worse, reverses triangle winding so back-face culling quietly removes the faces it should keep.
- Pre-rotation is applied here, from the left: ```rotate * proj * view * model```. It goes AFTER the Y flip so the rotation happens in y-down clip space, where a positive angle reads clockwise on screen — which is what ```ROTATE_90``` means in the spec. The four rotation matrices are built from exact integers rather than ```glm::rotate```, because ```cos(pi/2)``` in floating point is -4.4e-8 and these are meant to be exact axis permutations.
- There is no ```aspect``` field. It is derived from the extent inside ```ProjectionMatrix```, because under a 90 or 270 degree rotation the width and height the camera should reason about are SWAPPED relative to the swapchain's — and a settable field is an invitation to compute it from ```extent.width/extent.height``` somewhere else and get exactly that wrong. Getting it wrong looks like a bad FOV, not like a rotation bug.
- ```nearPlane```/```farPlane``` and not ```near```/```far```, which are macros in ```<windef.h>```. Depth precision lives in the RATIO far/near: pushing near from 0.1 to 0.5 buys more than pulling far from 1000 to 200.

---

## Light (cpp, putorana::graphics::Light)
- A light source as scene data, hanging off a Node. No Vulkan in it at all — not even an include. Whatever gathers lights each frame reads the fields and packs them.
- Position and direction are deliberately absent. Both come from the owning node's world matrix: position is its translation, direction is its -Z, the same convention as Camera and glTF. Storing a direction here would duplicate the node's rotation in a second place, and two copies of a rotation drift — and a spotlight could no longer just be parented to a torch.
- ```LightType``` is a plain field, not a virtual call. The consumer buckets lights by type and writes three differently shaped GPU arrays, so it is one switch at collection time then a ```static_cast```. Paying for virtual dispatch to ask "what are you" would be backwards for a data-oriented job.
- The copy constructor and assignment are PROTECTED on the base. Subclasses stay freely copyable, but assigning a ```SpotLight``` through a ```Light&``` — which would slice the cone angles off — will not compile.
- Colour is LINEAR RGB. It gets multiplied by intensity and summed with other lights, and both only mean anything in linear space; the sRGB conversion happens once, at the end of the frame.
- ```intensity``` means different things per type, which is the honest description rather than a leak: numerator of the linear ```intensity/distance``` falloff for Point and Spot, a plain multiplier for Directional, which does not attenuate because it is infinitely far away.
- Spot cone angles are in RADIANS while ```Camera::fovY``` is in DEGREES. Deliberate, and it comes from where the numbers originate: cone angles are read verbatim out of a glTF's ```KHR_lights_punctual```, which specifies radians; a field of view is a number a human types.
- ```cullable``` exists so a sun can opt out of frustum culling and not blink off when it leaves the frame. Nothing reads it yet.
- Being polymorphic is what forces ```Node::light``` to be a ```unique_ptr``` while ```renderable``` and ```camera``` are ```std::optional```. ```optional<Light>``` stores a Light by value and would slice a SpotLight's cone angles off — it would not even compile, since the constructor is protected precisely so a bare Light cannot exist. The storage choice follows from the type, not from taste.

---

## Material (cpp, putorana::graphics::Material)
- A shader plus its render state meeting a set of parameters, in two halves with two lifetimes. The TYPE (the subclass) decides the PIPELINE: shader, blend, cull, depth. The INSTANCE decides SET 2: its uniform buffer, its textures, its descriptor set. Two instances of one subclass differ there and nowhere else.
- ```CreatePipeline``` is ```const``` so the compiler enforces that rule: the result is shared between instances, so anything that changes render state has to be a different subclass.
- **The material builds the pipeline; the render pass owns it.** The WebGPU original caches pipelines in a ```static``` member of each material class, with a comment admitting the cache assumes one device for the life of the app. On Android that is false — the ```VkDevice``` dies every time the app is backgrounded — so a static cache would hand out pipelines belonging to a device that no longer exists, and the crash would land somewhere else entirely. The pass has the right lifetime, owns the attachment formats the pipeline needs, and makes a future shadow pass a second cache rather than a redesign.
- ```MaterialPipeline``` carries the pipeline AND its layout, because ```vkCmdBindDescriptorSets``` needs the layout and Vulkan — unlike WebGPU — does not let the pipeline supply it at the binding call.
- What an implementation owes: chain ```VkPipelineRenderingCreateInfo``` (there is no ```VkRenderPass``` to be compatible with), list the three set layouts in order, declare VIEWPORT and SCISSOR dynamic, and take its vertex input from ```VertexInputFor(format)``` so one material serves both a static and a skinned mesh.
- The set-2 layout is per INSTANCE rather than per class. Two identically defined ```VkDescriptorSetLayout``` objects are compatible in Vulkan, so a pipeline built with one binds a set allocated from the other — which buys away the whole static-lifetime problem for the cost of a duplicated handle.

---

## MeshPass (cpp, putorana::graphics::MeshPass)
- The opaque geometry pass. Not a ```VkRenderPass``` — dynamic rendering means no such object exists in this project; "render pass" is the logical unit: targets, descriptor layouts, and an algorithm for turning a tree of nodes into as few draw calls as possible.
- Four steps per frame. COLLECT walks the tree keeping renderables whose ```passMask``` has this pass's bit. SORT orders by pipeline, then material, then mesh. UPLOAD writes every object's matrices into set 1 in sorted order. DRAW walks the sorted list in runs sharing all three keys and emits one instanced draw per run.
- **The instancing identity**: slots in the object buffer are filled in the same order the draws are issued, and a draw's ```firstInstance``` is the index of its first slot. Vulkan defines ```gl_InstanceIndex``` as firstInstance plus the instance counter, so it lands exactly on the right slot. That is the whole scheme.
- Sorting is safe only because this pass is entirely opaque and the depth test settles visibility. In a pass with blending the order would be correctness, not a speed-up.
- Ordered by POINTER. The WebGPU version assigns first-appearance ids because JavaScript has no ordering on object references; ```std::less``` gives a total order over unrelated pointers, so the ids are unnecessary.
- Set 1 is a STORAGE buffer and not a uniform one: it is indexed at runtime and unbounded, and a uniform buffer caps at 16KiB on plenty of Android hardware, which is 128 objects.
- Sets 0 and 1 are rebound on every PIPELINE change rather than once before the loop. Binding once would only be legal while every material's pipeline layout stays compatible for those sets — true today, and the kind of invariant that breaks silently the day a material adds a push constant. The cost is one call per distinct pipeline, a handful per frame after the sort.
- Set 2 is rebound after a pipeline change even when the material did not change, because binding a layout disturbs the higher-numbered sets.
- Set numbering is by UPDATE FREQUENCY, which is the only thing it is good for: frame, then object, then material. Vulkan disturbs the sets numbered ABOVE one whose layout changed, so the most volatile goes last and a material switch cannot invalidate the camera.
- One colour target, not one per frame in flight, and the barrier is what makes that safe. A pipeline barrier's first synchronization scope covers everything earlier in SUBMISSION order, and submission order spans queue submits — so naming ```FRAGMENT_SHADER``` as the source stage is what stops this frame from overwriting an image the previous frame's final pass is still sampling.
- Depth is cleared and stored ```DONT_CARE```. On a tiler that is the difference between writing a full screen of depth out to RAM and never leaving tile memory with it.
- Growing the object buffer and rebuilding the targets both ```vkDeviceWaitIdle``` first. Rewriting a descriptor set the GPU may be reading is undefined; both events are rare (a doubling, a resize) so the blunt wait beats tracking which sets are live.
- Adding lights changes set 0's layout, and set 0's layout is in every material's pipeline layout — so it rebuilds every pipeline at once. Worth knowing before it looks like a small edit.
- Frustum culling belongs in COLLECT, before an object costs a pipeline lookup and a buffer slot. Not there yet; it is one line when there is a frustum extractor.

---

## DescriptorPool (cpp, putorana::graphics::DescriptorPool)
- Where every ```VkDescriptorSet``` comes from. A pool's budget is fixed at creation: guess low and allocation starts failing halfway through loading a scene, guess high and the memory is reserved regardless. Neither is worth getting right, so this grows — when a block refuses with ```OUT_OF_POOL_MEMORY``` or ```FRAGMENTED_POOL```, another block is created and the allocation retried. Any other result is a real out-of-memory and is reported instead.
- Sets are never freed individually, and the pools are deliberately NOT created with ```FREE_DESCRIPTOR_SET_BIT```: not asking for it lets the driver use a plain bump allocator inside each block. Everything dies with the Device anyway, so there is nothing to reclaim in between.

---

## Image (cpp, putorana::graphics::Image)
- A VMA image plus its one view — the counterpart of Buffer for attachments and textures.
- Never host-mapped, unlike Buffer. OPTIMAL tiling puts the memory layout in the driver's hands, so uploading pixels means a staging buffer and ```vkCmdCopyBufferToImage```. This is the one place "Android has unified memory so no staging is needed" does NOT apply: the obstacle is tiling, not locality.
- Attachments get ```VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT```. Full screen targets are large, tilers often want them on their own, and it stops one from pinning a whole suballocation block alive.
- The depth format is queried, never assumed. The spec guarantees only that at least ONE of ```D32_SFLOAT``` and ```X8_D24_UNORM_PACK32``` supports depth attachment, and never says which.

---

## Frame (cpp, putorana::graphics::DrawFrame)
- Free functions, no class. Drawing a frame is the renderer's job, and the renderer is this namespace rather than an object. Wrapping it in a ```class Renderer``` would only be a namespace with worse ergonomics.
- Runs once per vsync on the render thread. ```frameTimeNanos``` is when the frame is meant to be displayed, not when the call began, so animation driven from it stays smooth when a frame lands late.
- Swapchain recreation has two triggers and both matter. ```surfaceChanged``` is the obvious one. The other is ```VK_ERROR_OUT_OF_DATE_KHR``` or ```VK_SUBOPTIMAL_KHR``` coming back from acquire or present, and on Android that is the trigger that fires on rotation, because the compositor can change the surface transform without the window size moving at all.
- Those two results need opposite handling. ```OUT_OF_DATE``` from acquire leaves the semaphore unsignalled, so the frame can be dropped on the spot. ```SUBOPTIMAL``` signals it, so the frame has to be finished normally or that semaphore stays signalled with nobody waiting on it.
- ```vkQueueSubmit2``` signals the binary present semaphore and the timeline value from one array. Before synchronization2 the timeline values lived in a parallel struct whose arrays had to be kept index aligned by hand.
- Dynamic rendering, so there is no ```VkRenderPass``` and no ```VkFramebuffer``` anywhere in the project.
- Drives the world when there is one: ```Update``` before the acquire (it touches no Vulkan, only nodes and matrices, so it has nothing to wait for), then ```Render``` between the two layout barriers. With no world installed it falls back to the slow colour sweep it always had, which is what proves acquire/submit/present work before there is anything to draw.

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