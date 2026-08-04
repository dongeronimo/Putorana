# putorana::recon

Turning what the camera measures into a surface we can draw and collide against.

This document was written before any of the code was, because every mistake it
warns about produces a reconstruction that *looks* like a plausible room and is
wrong. Most of it still stands. The parts that a device settled are marked
MEASURED, and the one question it left open has since been closed.

It runs. On a Samsung SM-S731B: 30 fps, integration 19 ms with 111 chunks in
view, marching cubes sharing vertices at 4.69x, 4 MiB of mesh for 64 chunk nodes.
The story of how it got there, including the several changes that looked right
and were not, is in `reconstruction-quality.md` at the repository root.

---

# The decision, and why

Two options were on the table: use OpenChisel, or write the algorithm ourselves.

**The plan is OpenChisel now, and moving pieces of it onto compute shaders over
time until there is nothing of it left.** That is not a compromise between the two
options. It is a sequence. The library buys a working reconstruction to look at
and argue with; each piece that turns out to matter gets replaced deliberately,
with the original still running beside it to check the replacement against. The
alternative was writing all of it before seeing any of it work, and being unable
to tell a maths bug from a plumbing bug when the result looked wrong.

OpenChisel is the reference implementation of the CHISEL paper (Klingensmith et
al., RSS 2015), written for Google Tango. It was measured before choosing: the
core library is **4814 lines across 16 .cpp files, depending only on Eigen and
the STL**, with no ROS and no PCL, despite the catkin packaging. It compiles
clean for arm64-v8a under NDK 28 at `-std=c++17`, unmodified. It is MIT licensed.

## What was wrong with it, and what we did about it

The objections were real. They were also, every one of them, about 2014 C++
rather than about the algorithm, which is why forking beat rejecting:

- **Its memory layout was wrong for a phone.** `DistVoxel` had a *virtual
  destructor*, so on arm64 every voxel carried an 8-byte vtable pointer next to
  its 4-byte SDF and 4-byte weight: 16 bytes where 8 would do. `ColorVoxel` was
  the same, 16 bytes to store 4 of RGBA. A voxel cost 32 bytes instead of 12,
  and half of every cache line the integrator touched was vtable pointer. Nothing
  was ever used polymorphically through those pointers: the entire codebase
  contains exactly **three** inheritance relationships.
- **`Truncator` and `Weighter` dispatched virtually from inside the per-voxel
  loop.** Their bodies are a handful of arithmetic operations, so the cost was
  not the dispatch but the inlining, hoisting and vectorisation it prevented.
  `QuadraticTruncator` computed its square as `pow(reading, 2)`, which behind a
  virtual boundary could not be folded, so squaring a float was a libm call, per
  voxel.

Both are fixed in the fork at `third_party/open_chisel/`, whose README documents
every change and the measurements behind them. A voxel is now **12 bytes**, the
library emits **no vtables at all**, and the compiled output contains no calls to
`pow`.

### Where we diverged after it was running

The two changes above were made before anything had been executed, on the
strength of reading the code. The three below came out of watching it fail on a
device, and each is a place where upstream's behaviour is not merely dated but
wrong for what we are doing. They are listed here as well as in the fork's README
because they are the reason the fork cannot be dropped for a fresh checkout.

**1. Marching cubes emitted a triangle soup.**
`MarchingCubes.h`, `MarchingCubes.cpp`, `ChunkManager.cpp`.

Every triangle allocated three fresh vertices, so a vertex where six triangles
meet was stored six times with six identical normals, and the index buffer
carried nothing beyond `0, 1, 2, 3, ...`. Upstream asserts the property outright:
`vertices.size() == indices.size()`.

We now key each vertex on the **grid edge** it was interpolated on. The cube
corner offsets plus the existing `edgeIndexPairs` give the axis and lower
endpoint of any edge, which packs into a 64-bit key, so two cubes sharing an edge
produce byte-identical keys. Keying on topology rather than on position is the
part that matters: a position key needs an epsilon, and an epsilon on
interpolated floats either merges vertices that should stay distinct or fails to
merge ones that should not.

Measured 4.08x to 4.69x fewer vertices for the same geometry, and mesh memory for
a room fell from 17 MiB across 263 nodes to 4 MiB across 64.

**2. The depth integration path ignored its own weighting policy.**
`ProjectionIntegrator.h`.

`ProjectionIntegrator::Integrate` passed a hardcoded `1.0f` as the weight. The
`weighter` member is consulted only from `IntegrateColor`, which this app never
calls, so a configured `Weighter` did nothing at all in the live path.

Calling the weighter would fix the lie but not the need. ARCore ships a
confidence map alongside raw depth, and its samples are not of equal quality. The
weighter's signature, `GetWeight(surfaceDist, truncation)`, has nowhere to put a
per-pixel quantity. So the integrator now takes an optional image of per-pixel
weights, indexed at the same row and column the depth was read from. Null
restores upstream behaviour exactly.

Why weighting instead of filtering the depth map before handing it over: covered
under *"Two ways to use it, and only one of them works"* below. The short version
is that a hard threshold deletes floors.

**3. Remeshing was all-or-nothing.**
`Chisel.h`, `GetMutableMeshesToUpdate()`.

Upstream's only way to remesh is `UpdateMeshes()`, which does every dirty chunk
and then clears the set. Integration marks the whole 3x3x3 neighbourhood of every
chunk it touches, so sweeping a room dirties hundreds at once and meshing all of
them inside one frame is a dropped frame. Mutable access lets `Remesh` take a
bounded number per frame and leave the rest marked.

That change hands the caller a scheduling problem that upstream never had, and
the obvious way to schedule it starves. See *"The remesh cycle"* below.

### What is still wrong with it, deliberately

Listed in the fork's README rather than fixed, so that the diff stays reviewable.
Two of them are behavioural and cost real time:

- **`Chisel::IntegrateDepthScan` builds its frustum from the maximum value in the
  depth image**, ignoring the camera's configured far plane. Worked around in our
  conversion loop rather than fixed. See *"The far plane trap"* below, because
  anyone touching that loop needs to know why the clamp is there.
**4. Space carving could not remove anything.**
`DistVoxel.h`, `ProjectionIntegrator.h`.

Four independent reasons, any one of them sufficient: `Carve` added weight to the
voxel it was erasing, it pulled the SDF toward the surface rather than toward
free space, its eligibility test admitted only voxels already at or behind a
surface, and the branch was unreachable at the configured distance because
`carvingDist` was smaller than the padding the integration branch tests against.

Carving now decays the accumulated weight and resets the voxel to unobserved,
the padding is one voxel diagonal rather than two, and `carvingDist` is a dead
zone above the integration band rather than a threshold competing with it. The
fork's README has the full derivation. A weight ceiling came with it, since an
unbounded running mean is the other reason nothing could ever be corrected.

The small ones: `size_t` mesh indices feeding a `VK_INDEX_TYPE_UINT32` buffer,
`shared_ptr<Chunk>` in the hash map putting an atomic refcount in every lookup,
and Eigen where the rest of this project is GLM.

`Threading.h::parallel_for` is *not* on that list, despite appearances. Its
`threshold = 1000` default means it spawns no threads at all below ~1000 chunks
in the frustum, which is where we operate, so the integrator is single-threaded
today whether or not anyone intended it. The fork's README has the arithmetic.
Worth knowing mainly so nobody spends an afternoon tuning a thread count that
does nothing.

## The boundary, and why it is also a build setting

Eigen is an expression-template library: unoptimised, every vector operation
becomes a chain of real function calls instead of collapsing into a few
instructions, and `eigen_assert` bounds-checks each access. A debug build of
anything Eigen-heavy is not a bit slower, it is *categorically* slower. Slow
enough that you stop being able to tell a performance bug from the debug build,
which is how afternoons disappear.

On Android we can do something about that which Windows does not allow: build
this one library with full optimisation while the app around it stays debuggable.

The reason the Windows intuition does not transfer is specific. There, debug
and release are **different C runtimes**, with different heaps, so memory
allocated in one and freed in the other corrupts; and `_ITERATOR_DEBUG_LEVEL`
changes the *layout* of standard containers, so a `std::vector` is not even the
same type across the boundary. Both are ABI breaks. Android has one libc++ and
one bionic allocator for the entire process, and optimisation level is not an
ABI property.
Mixing `-O0` and `-O3` translation units is ordinary there.

The one real hazard is ODR, and it has a name: **`NDEBUG`**. Eigen turns `NDEBUG`
into `EIGEN_NO_DEBUG` (`Macros.h:987`), which removes `eigen_assert` from inline
template code. Compile the same Eigen header into two translation units with
different `NDEBUG` and you emit two different definitions of the same weak
symbol; the linker keeps whichever it saw first. That is undefined behaviour, and
its symptom is that you sometimes get the checked version and sometimes the fast
one, with no pattern.

So the rule is not "compile the library fast", it is **Eigen must never be
visible outside the translation units compiled with those flags.** Which makes
this namespace a *facade*, and turns a build setting into an architectural
constraint:

  * `putorana::recon` compiles as its own static library, with `NDEBUG` and `-O3`
    even in Debug builds.
  * Its `.cpp` files are the **only** places in the project that include
    `<open_chisel/...>`, and therefore the only places Eigen is ever parsed.
  * Its public headers expose GLM and plain structs. The main library physically
    cannot see an `Eigen::Vector3f`, because nothing it includes mentions one.

The payoff is that this is the same seam the migration needs. Replacing a piece
of OpenChisel with a compute shader is a change *behind* the facade, and nothing
outside this namespace has to know it happened.

## Which piece migrates first, and the reason is lifetimes

The tempting first piece to move onto the GPU is the voxel volume itself. It is
the wrong one, and the reason is written into this project's architecture
already.

The root README's rule: *"Nothing may be loaded once and cached across
surfaces."* The `Allocator` dies with the `Device`, and the `Device` dies **every
time the app is backgrounded**. A voxel volume living in `VkDeviceMemory` would
be destroyed every time the user presses Home, throwing away the reconstructed
world, which is the one thing in this app that must survive. The workarounds are
to re-upload the whole volume on every resume, or to keep a CPU mirror; and if
there is a CPU mirror, the CPU version is already written.

`ar::Subsystem` solved exactly this problem already, and correctly: it lives at
process scope because destroying the session throws away tracking. The voxel grid
has the same property and belongs at the same level.

The second reason is arithmetic. ARCore's depth map is small, **measured at
160x90 on the device this was developed against**, up to 640x480 on some others.
That is 14,400 samples per frame; at 20-40 voxels touched per ray, a few hundred
thousand voxel updates. That is milliseconds on one arm64 core. The expensive part of a GPU implementation is not the
integration kernel. It is dynamic chunk allocation on the GPU (a hash table with
atomics, a free list, garbage collection), which is where all the difficulty and
all the bugs live, in exchange for speeding up the part that was never slow.

The part that *will* hurt is marching cubes over dirty chunks and re-uploading
their vertex buffers. **That** is the good compute-shader candidate, later: it is
pure, wide, and needs no dynamic allocation if the output per chunk is capped.

None of the CPU work is throwaway. The chunk layout chosen for it is the same
layout that would be uploaded to the GPU, and the CPU implementation becomes the
**oracle** the compute version is diffed against.

---

# The input

Everything below comes from `ar::CameraFrame`, in `putorana/ar/Subsystem.h`.
`putorana::recon` must not include `volk.h`, the same structural rule
`putorana::ar` follows: plain memory in, plain vertex arrays out, and
`putorana::graphics` turns those into a `Mesh`.

## Which pose, and the trap this section exists for

ARCore hands out **two** poses, and they differ by a rotation about the view axis
of a multiple of 90 degrees. Picking the wrong one does not make the
reconstruction subtly off. It lays the room on its side.

| | `displayPose` | `sensorPose` |
|---|---|---|
| ARCore call | `ArCamera_getDisplayOrientedPose` | `ArCamera_getPose` |
| "right"/"up" relative to | the logical **display** | the sensor's **readout order** |
| pairs with | `projection` | `imageIntrinsics`, `depth` |
| used by | `graphics::ArCamera` (rendering) | **this namespace** (reconstruction) |

The rule that decides it: **the CPU image and its intrinsics are unrotated**, and
ARCore says so in as many words. `ArCamera_getImageIntrinsics` returns "the
unrotated and uncropped intrinsics for the image (CPU) stream", and the
`AR_COORDINATES_2D_IMAGE_*` spaces are documented as the CPU image while the
`VIEW` ones are explicitly "display-rotated". Pixels that were never rotated must
be unprojected with a pose that was never rotated either.

### This is not a bug in the render path

Worth stating plainly, because the natural question is whether `graphics::ArCamera`
should have been using the intrinsics all along. It should not, and nothing there
needs fixing.

The two halves of the app ask **inverse questions**:

- Rendering asks *"where on screen does this 3D point land?"*, which is 3D to
  2D. ARCore answers it with a finished `projection` matrix. The intrinsics are
  already inside that matrix; that is what a projection matrix *is*. They are
  absent from the render path because they are already there, in matrix form.
- Reconstruction asks the opposite: *"this pixel reads 2.3m, where in 3D is
  that?"*, which is 2D to 3D. That is the inverse of a projection, it cannot be
  done with a projection matrix that has already discarded depth, and it
  operates on a **different image** (the depth map, unrotated) under a
  **different rotation convention**.
So the two are not redundant descriptions of one camera. They are two operations,
in opposite directions, on two images with two conventions.

## What the depth map contains

`ArFrame_acquireDepthImage16Bits`, format `HardwareBuffer.D_16`: a single 16-bit
plane, little-endian, each sample an unsigned count of **millimetres**, range
0..65535mm (~65m). A zero means *no estimate here*, not *zero distance*.

There are no confidence bits. (The **deprecated** `ArFrame_acquireDepthImage` was
the one that packed 13 bits of depth and 3 of confidence into a `DEPTH16`; the
16-bit call exists precisely to stop doing that, which is what "expands the depth
range from 8191mm to 65535mm" in its deprecation notice means.)

**The sample is Z, not range.** ARCore's wording: "the distance in millimeters
along the camera principal axis". It is *not* the length of the ray from the
optical centre to the point. This is the convenient case and the next section
shows why. But code written for a time-of-flight sensor, which usually reports
range, is wrong here in a way that bulges the reconstruction outward toward the
frame edges while looking perfect at the centre.

Accuracy, from ARCore: best between 0.5m and 15m, usable to 25m, **error growing
quadratically with distance**. That quadratic is not a footnote. It is the reason
a TSDF weights near samples above far ones, and it is where `QuadraticTruncator`
in OpenChisel comes from.

### Reading a sample

`rowStrideBytes` is what ARCore reports and it is in **bytes**, while the pointer
is `uint16`. Rows may be padded. So:

```cpp
const uint16_t* row = depth.millimetres + y * (depth.rowStrideBytes / 2);
const uint16_t  mm  = row[x];
if (mm == 0) continue;              // no estimate
const float d = mm * 0.001f;        // metres
```

`depth.millimetres[y * width + x]` is correct only when the rows happen not to be
padded, which is exactly the kind of thing that works on the development device
and not on the next one.

### Automatic vs raw, and the confidence image: SETTLED

There are two depth streams. We read **raw**, together with its confidence map.

| | **Automatic** | **Raw** (what we read) |
|---|---|---|
| call | `ArFrame_acquireDepthImage16Bits` | `ArFrame_acquireRawDepthImage16Bits` |
| coverage | dense, every pixel has a value | sparse, 0 where ARCore is unsure |
| filtering | smoothed, temporally fused by ARCore | "mostly unfiltered" |
| confidence | none | `ArFrame_acquireRawDepthConfidenceImage` |

This section previously held the choice open, with density as the argument for
automatic. The device closed it, and both halves of the reasoning are worth
keeping because each is a different failure.

**Against automatic.** A TSDF performs its own temporal fusion. Integrating a map
ARCore has already fused means fusing the same measurement twice, so the weights
claim more independent evidence than was collected. Worse, "depth estimation for
every pixel in the image" is more than any sensor measures, so the difference is
smoothing plus inpainting of what was never observed. Averaging correlated data
converges on the smoothing rather than on the surface, and the mean of a hundred
views of the same guess is the guess.

**Raw is not free of that problem, it is honest about it.** Raw depth is motion
stereo: ARCore matches image patches between successive frames and turns
disparity into distance. On a plainly painted wall there is nothing to match, the
matching cost is flat across the whole disparity search, and the minimum falls
out of sensor noise rather than geometry. Those errors are correlated between
nearby viewpoints exactly like the smoothed stream's are, so fusing more frames
entrenches them.

The difference is that raw depth **says which samples those are**. That is what
the confidence image is for, and using raw without it is using half the API.

### The confidence map: MEASURED

`ArFrame_acquireRawDepthConfidenceImage`, format `Y8`: one byte per depth sample,
0 for lowest and 255 for highest, same dimensions as the depth map. ARCore
documents all three of its depth outputs as identically sized, and
`ar::Subsystem` checks that rather than assuming it, because a confidence map
read at the wrong stride masks a drifting diagonal band of the depth map instead
of the pixels it describes.

Its distribution is the thing to know, and it is not what the linear 0-255 range
suggests. Measured over sixty-frame windows while sweeping a room:

```
RECONPROBE confidence of the samples that DID exist, 32 wide: 40% 3% 3% 4% 4% 6% 4% 34%
```

**Bimodal.** Roughly 40% in the bottom bucket, roughly 34% in the top, and the
six middle buckets holding 3% to 6% each. ARCore is close to binary about whether
it knows a distance. Any weighting curve built on the assumption of a smooth
spread is mostly operating on empty range.

Separately, the fraction of samples with **no estimate at all** runs 6% to 25%
under normal sweeping, rising above 70% when the phone is nearly still. That
number matters because it bounds what any amount of filtering can achieve: a
sample ARCore did not produce cannot be recovered by weighting.

### Two ways to use it, and only one of them works

Google's guidance is a hard threshold:

> If an application requires filtering out low-confidence pixels, removing depth
> pixels below a confidence threshold of half confidence (128) tends to work
> well.

Tried, measured, rejected. At 128 it discarded **79% of the samples in a frame**.
The reconstruction became noticeably more faithful and unusable: a tiled floor
came back as a scatter of disconnected islands rather than a surface.

That is not a threshold in need of adjustment. A floor is precisely the
low-texture surface the confidence map is most pessimistic about, so any
threshold high enough to suppress the bad geometry also deletes the good
geometry in the same regions.

A TSDF fuses by weighted average, which gives a better instrument. A poor sample
can be admitted with a small weight, so it builds a continuous surface where
nothing better exists and is outvoted wherever something better arrives. So the
two mechanisms in `Config` do different jobs:

| field | job |
|---|---|
| `confidenceThreshold` (32) | outlier rejection. Keeps absurd readings out of the **frustum**, which is the one thing a weight cannot do. |
| `confidenceWeightFloor` (0.05) | quality. Confidence 32..255 maps linearly onto weight 0.05..1.00. |

The floor is deliberately not zero. Zero collapses the mechanism back into the
gate that produced the islands.

The gate is applied **before** the furthest-sample bookkeeping, and that ordering
is not cosmetic. See *"The far plane trap"* below.

---

# The mathematics

## Intrinsics

Four numbers describe the entire 3D-to-2D map of a pinhole camera. For a point
`(X, Y, Z)` in camera space, with `Z` measured along the principal axis:

```
u = fx · (X / Z) + cx
v = fy · (Y / Z) + cy
```

`fx`, `fy` are the focal length expressed **in pixels**, in pixel widths and
pixel heights respectively. They differ only when the sensor's pixels are not
square, which on a phone they essentially always are, so expect `fx ≈ fy` and
treat a large gap as a bug rather than as a property of the hardware.

`cx`, `cy` are the **principal point**: where the optical axis actually pierces
the sensor. Near the image centre, never exactly at it, and that small offset is
why ARCore's projection matrix carries terms outside the diagonal. See the
comment in `graphics/ArCamera.cpp` about negating the whole row rather than just
`[1][1]`.

A useful sanity check, since `fx` in pixels is hard to have intuition about:

```
horizontal FOV = 2 · atan(width / (2 · fx))
```

A phone's main camera should come out somewhere around 60-70°. If that number is
absurd, the intrinsics do not belong to the image they are being used with.

### Rescaling: a crop, then a scale: MEASURED

`ArCamera_getImageIntrinsics` describes the **CPU image**. The depth map is a
different, smaller image. Using one against the other builds a reconstruction
that is uniformly, consistently, plausibly the wrong size.

This section used to say the fix was a plain multiply by the width and height
ratios, on the assumption that depth covered the same field of view as the CPU
image, flagged as unverified. **A device settled it, and the assumption was
wrong.** Measured:

```
RECONPROBE depth 160x90, stride 320 bytes, tight would be 320 -> TIGHT
RECONPROBE aspect depth 1.7778 vs CPU image 1.3333 (image 640x480) -> MISMATCH
RECONPROBE intrinsics image fx 439.73 fy 440.19 cx 314.88 cy 241.35
```

16:9 depth from a 4:3 image. The depth map is **cropped vertically**, not merely
resampled.

The failure mode of getting this wrong is worth dwelling on, because it is the
kind that survives review. Scaling `fy` by `depthHeight / imageHeight` preserves
the field of view of the image you scaled *from*, so the maths ends up believing
the depth map sees 57.2° vertically when it really sees 44.5°. Nothing looks
broken. The room simply comes out with its floor and ceiling flared away from the
camera, worsening toward the top and bottom of frame. Every other check in the
probe reported fine.

The correct model is a centred crop to the depth map's aspect ratio, then a
uniform resample:

```
if depthAspect > imageAspect:  cropH = imageW / depthAspect   (lose rows)
else:                          cropW = imageH · depthAspect   (lose columns)

cropLeft = (imageW − cropW)/2        cropTop = (imageH − cropH)/2
s        = depthWidth / cropW        ( == depthHeight / cropH )

fx' = fx · s                cx' = (cx − cropLeft) · s
fy' = fy · s                cy' = (cy − cropTop)  · s
```

The crop is derived from the two aspect ratios rather than hardcoded, so a device
that crops the other way, or not at all, falls out of the same three lines.

Two things in there are easy to get wrong:

- **`cx` and `cy` are positions, not lengths.** The crop origin has to come off
  them *before* the scale. This is invisible on a perfectly centred sensor,
  because `cx − cropLeft` lands back in the middle anyway, and no real sensor is
  perfectly centred.
- **`s` is one number.** Because the crop restores the aspect ratio before the
  resample, the horizontal and vertical scales are equal. That is the invariant
  worth asserting: if they ever differ, the crop is not centred and this model is
  wrong for that device.

On the measured numbers: `cropTop = 60`, `s = 0.25`, `fx 439.73 → 109.93`,
`fy 440.19 → 110.05`, `cx 314.88 → 78.72`, `cy 241.35 → 45.34`. The sensor's
off-centre principal point (+1.35 px of 480) survives as +0.34 px of 90, exactly
a quarter, which is independent confirmation that the crop really is centred.

`ar::Subsystem::ReadDepthImage` does this and hands over the result as
`depth.intrinsics`. `frame.imageIntrinsics` is kept alongside it so the two can be
compared when something looks wrong.

### The check that actually catches this

Every individual number above can look reasonable while being wrong. One
relationship cannot:

```
tan(hfov/2) / tan(vfov/2)  ==  width / height
```

Both sides equal `(width/2)/fx` over `(height/2)/fy` for square pixels, so it ties
`fx`, `fy`, `width` and `height` together in a single number that no one of them
can be wrong without disturbing. With the old height-ratio rescale it came out
**1.335 against a 1.778 image**, a 25% error, on a frame where the horizontal
FOV, the stride, and the principal point all read as fine.

`Subsystem` computes it on the first depth frame and states the verdict itself.
Search logcat for `RECONPROBE`; the last line is `VERDICT OK` or `VERDICT FAIL`.
It is the one number to look at when a reconstruction comes out subtly the wrong
shape.

## Unprojection

The operation this whole namespace is built on: pixel + depth → a 3D point in the
world.

### Step 1: pixel to camera space

Invert the projection. Given pixel `(u, v)` and depth `d` metres, and knowing
`Z = d` because the depth is along the principal axis:

```
X = (u - cx) · d / fx
Y = (v - cy) · d / fy
Z = d
```

**This is where Z-versus-range earns its keep.** The ray through pixel `(u,v)` has
direction `((u-cx)/fx, (v-cy)/fy, 1)`. Note that the third component is exactly `1`,
un-normalised. Because `d` is measured along Z, scaling that un-normalised
direction by `d` lands on the point directly. Had ARCore given range instead, the
direction would first have to be normalised, costing a square root per pixel, and a
different answer.

### Step 2: into ARCore's axis convention

The formulas above are the computer-vision convention: **+X right, +Y down, +Z
forward**, because `v` counts downward from the top of the image.

ARCore's camera pose is the OpenGL convention: **+X right, +Y up, −Z forward**
("+X pointing right, +Y pointing up, and −Z pointing in the direction the camera
is looking"). Y and Z both flip:

```
X_cam =  (u - cx) · d / fx
Y_cam = -(v - cy) · d / fy
Z_cam = -d
```

Check the signs by reasoning about one point rather than trusting the algebra: a
pixel *below* the optical axis has `v > cy`, and it is physically *below* where
the camera points, so its `Y` must be negative when +Y is up. And every visible
point is in front of the camera, which is the −Z direction, so `Z` is negative.

### Step 3: camera space to world space

`sensorPose` is a rotation and a translation. With `q` its unit quaternion and `t`
its translation:

```
P_world = q · P_cam · q⁻¹ + t
```

In GLM, `glm::quat(w, x, y, z) * P_cam + t`. ARCore's raw order is `qx, qy, qz,
qw` and glm's constructor takes **w first**, the same reordering already
commented in `graphics/ArCamera.cpp`, and the same trap. Getting it wrong yields
a rotation that looks plausible and that no single still frame reveals as wrong.

### The check to run first

Take the centre pixel, `u = cx`, `v = cy`. Then `X_cam = Y_cam = 0` and
`Z_cam = -d`: a point straight down the optical axis at distance `d`. Transform it
by `sensorPose` and it should sit exactly where the phone is pointing, `d` metres
away. That single point, drawn as a marker, is the first thing worth building,
before any voxel exists.

## Rotation, and why it never appears in this namespace

The obvious question, having read the above: don't the intrinsics have to be
rotated to match the display at some point, if not now then when the mesh is
finally drawn?

No. Not now and not later. The display rotation never enters the reconstruction
pipeline at all, and the reason is one word in ARCore's header: the two poses
differ by a **local** rotation about Z. Local to the camera's own axes. Both are
returned "in world space", and the world frame is identical for both.

That makes two pairings self-consistent, and they produce the *same world point*:

| depth pixels | intrinsics | pose | result |
|---|---|---|---|
| as delivered | unrotated | `sensorPose` | world point P |
| rotated 90° | rotated 90° | `displayPose` | the same world point P |

The rotation cancels: rotating the image one way obliges you to rotate the pose
the other way, and composed in world space that is the identity. We take the
pairing in which nothing needs rotating.

Which is also why rotating the intrinsics *on their own* would be a bug rather
than a step in the right direction. It is only meaningful together with rotating
how the depth pixels are indexed **and** switching to `displayPose`, so three
changes that arrive at the number we already had.

```
depth pixel ──[unrotated intrinsics]──▶ sensor camera space
            ──[sensorPose]───────────▶  WORLD SPACE  ◀── reconstruction lives entirely here
                                             │
                                             │  marching cubes emits world-space vertices
                                             ▼
   screen ◀──[displayPose · projection · preTransform]──
```

World space is the pivot, and the two rotation conventions sit on opposite sides
of it and never meet. Marching cubes emits world-space vertices; they become a
`Mesh` and are drawn by `MeshPass` through `graphics::ArCamera`, which already
applies ARCore's display-aware projection and the swapchain's `preTransform`.
That is the code that makes the cube line up with the camera image today, and the
reconstructed surface is simply more geometry going through it. **The display
rotation is handled exactly once, at render time, by code that already exists.**

A corollary worth relying on: this namespace is **immune to display rotation**.
The manifest pins the activity to portrait, but the root README notes that
`preTransform` may still be non-identity and that API 36 ignores that restriction
on large screens. None of it reaches here, because nothing here reads a
display-oriented quantity. Unlocking landscape would change nothing in `recon`.

### The exception, and it is already solved

One operation genuinely does need view-to-image: sampling depth at a **screen**
pixel, for tap-to-place or for occluding virtual content against real depth. That is
the reverse direction, from something the user touched to something the sensor
measured.

Even there, nothing is rotated by hand. `ArFrame_transformCoordinates2d` maps
`AR_COORDINATES_2D_VIEW_NORMALIZED` to `AR_COORDINATES_2D_IMAGE_NORMALIZED`, and
`ar::Subsystem` already evaluates it every frame and hands it over as
`CameraFrame::viewToImage`. Multiply the resulting normalised image coordinates
by the depth map's width and height and you have the pixel to sample.

---

# The representation: a TSDF

Unprojection gives points. Points are not a surface, and a million of them from a
thousand frames are not a better surface. They are a thousand noisy opinions
about the same wall with no way to reconcile them. The TSDF is how they get
reconciled.

**TSDF** is *Truncated Signed Distance Field*, and the quickest way in is to read
the name backwards.

**Field.** Rather than storing the surface, store **a number at every point in
space**, sampled on a 3D grid of voxels. The surface is never stored at all: it is
*implied*, as the place where that number crosses zero. This is an **implicit
surface**.

**Distance.** The number is the distance to the nearest surface. Zero means the
surface is exactly here.

**Signed.** The sign says which side you are on. The convention used throughout
reconstruction, and the one this namespace will follow: **positive in free
space**, between the camera and the surface; **negative behind the surface**,
inside the wall where the camera cannot see; zero on the surface itself. It is
only a convention, and flipping it flips every normal in the output mesh, but it has
to be picked once and never wavered on.

**Truncated.** Values are stored only in a thin band around the measured surface,
say ±12cm. Outside the band, nothing. Two reasons, both of which matter:

1. *Beyond the band there is nothing honest to store.* One depth pixel taught us
   "there is a surface 2m along this ray". It taught us nothing about a voxel
   50cm off to the side, whose nearest surface might be something else entirely.
   Writing a value there would be inventing data.
2. *Memory becomes proportional to surface area rather than to volume.* A room is
   mostly air. The truncation is what stops us paying for the air.

## Why a field rather than a surface

This is the property that makes the whole method work, and it is worth being
explicit about.

Roughly sixty depth maps arrive per second, each one noisy, most of them looking
at the same wall. They have to be combined somehow.

- **As meshes:** work out which triangles from frame 2 overlap which from frame 1,
  stitch the seams, resolve the contradictions. That is hard geometry, and it
  gets harder with every frame added.
- **As a field:** `+=` into a grid.

Fusing 3D observations becomes **a running weighted average of scalars**, which is
about the easiest operation in computing. Noise averages away. Holes fill in as
new data arrives. Topology resolves itself, with no stitching, no overlap tests,
nothing.

## One ray, with numbers

Voxels of 4cm, truncation τ = 12cm. A depth pixel reads **d = 2.00m**. Walk the
ray and store `sdf = d − t` at each voxel `t` metres along it:

| t (m) | sdf = d − t | meaning |
|---|---|---|
| 1.84 | none | outside the band, untouched |
| 1.88 | +0.12 | band edge, free space |
| 1.92 | +0.08 | free space |
| 1.96 | +0.04 | free space, closing in |
| **2.00** | **0.00** | **the surface** |
| 2.04 | −0.04 | behind the surface |
| 2.08 | −0.08 | behind |
| 2.12 | −0.12 | band edge |
| 2.16 | none | outside, untouched |

## The fusion rule

The next frame measures the same wall at **2.02m**, the same wall at a different
number, because the sensor is noisy. The voxel at t = 2.00 receives a new
observation of +0.02. With both weights at 1:

```
D_new = (1 × 0.00 + 1 × 0.02) / (1 + 1) = +0.01
W_new = 2
```

The zero crossing has moved to 2.01m, halfway between the two measurements. After
a hundred frames the surface sits at the mean of a hundred noisy readings, and is
considerably more accurate than any single depth map ARCore produced.

That is the entire fusion step, and it is four lines. Quoted from OpenChisel's
`DistVoxel::Integrate`, which is the reference implementation this namespace is
written against:

```cpp
float newDist = (oldWeight * oldSDF + weightUpdate * distUpdate) / (weightUpdate + oldWeight);
SetSDF(newDist);
SetWeight(oldWeight + weightUpdate);
```

An incremental weighted mean. It dates to Curless & Levoy, 1996.

Note that the distance is **normalised** and the weight is not. That division is
what keeps the stored number a distance in metres. An un-normalised weighted sum
would grow without bound: its zero crossing would still be findable, but the
magnitude would stop meaning anything geometric, and everything that reads the
magnitude would break: interpolating where the surface crosses a
marching-cubes edge, gradients, clearance queries.

### Where the distance-dependence actually lives

The weight is where sensor knowledge enters, but not as a free parameter. In
OpenChisel it is *coupled to the truncation distance*, and the coupling is worth
copying:

```cpp
// QuadraticTruncator: the band grows quadratically with the reading
τ(d) = |a·d² + b·d + c| · scale

// ConstantWeighter, despite the name
GetWeight(...) = weight / (2·τ)
```

"Constant" means constant *total* vote, not constant per voxel. Every measurement
deposits the same total weight; a distant one spreads that total over a thicker
band, so each individual voxel receives less of it. Far, uncertain measurements
lose influence **as a consequence of being uncertain**, rather than because
someone tuned a falloff curve to make them.

Two knobs would have to be kept consistent by hand. One knob, with the weight
derived from it, cannot drift out of agreement with itself.

(`ConstantWeighter::GetWeight` also receives `surfaceDist`, the position within
the band, and ignores it. That is the hook for weighting the front of the band
above the back, an observation of free space being more trustworthy than a guess
about what is behind a surface. Unused in OpenChisel; worth remembering.)

## Getting a surface back out

Marching cubes. Take each cube of 8 neighbouring voxels and look at the **signs**
at its corners. Two corners joined by an edge with different signs means the
surface crosses that edge, and where it crosses is found by linear interpolation
between the two values. 8 corners give 256 possible sign patterns, which is a
lookup table, which gives triangles.

Depth maps in, weighted averages in a grid, zero crossings out as a mesh.

## The approximation hiding in `d − t`

`sdf = d − t` is the **projective** distance: measured along the camera ray,
rather than perpendicular to the surface. The two agree only when the surface is
viewed head-on. At a grazing angle the ray distance *overestimates* the true
distance to the surface, which biases the result.

It is done anyway, universally, because it is nearly free. What matters is
knowing that the number in a voxel is an approximation of a distance rather than
a distance.

## What CHISEL contributes, and what it inherits

Useful framing when reading the paper, because it spends little time on the part
described above.

None of it is CHISEL's. The TSDF is Curless & Levoy (1996); doing it live from a
depth camera is KinectFusion (2011). Both assume a **dense** voxel grid inside a
bounding box declared up front, "reconstruct this 4×4×4m cube", and allocate
all of it, air included.

CHISEL's contributions are about making that survive on a phone, in a scene with
no declared size:

- **spatial hashing of chunks**, small blocks of voxels allocated only where
  surface actually appears, so empty space costs nothing at all, not even a hash
  entry, and there is no bounding box to declare in advance;
- **dynamic truncation**, where τ grows with distance because depth error grows
  with distance (quadratically, for ARCore, which is where `QuadraticTruncator`
  comes from); - **space carving**, actively clearing voxels observed to be
  empty, so that
  something which moves away does not leave a ghost behind.

So when the paper concentrates on hashing and chunk management rather than on the
distance field itself: the field is the inherited part, and the storage is the
new part. The storage is also the part this namespace has to get right, since it
is the reason we are writing it ourselves rather than taking OpenChisel whole.

---

# The library, as it exists

## The shape of the API

One object, at process scope, driven once per frame.

```cpp
recon::Config config;
recon::reconstruction_holder::Create(config, error);

// per frame, under the ar::Subsystem frame guard
reconstruction->Integrate(frame.depth, frame.sensorPose);
reconstruction->Remesh(kRemeshBudgetPerFrame);
reconstruction->CollectUpdates(changed, removed);
for (const ChunkUpdate& u : changed) {
    reconstruction->WriteChunk(u.key, vertices, vertexCapacity, indices, indexCapacity);
}
```

`reconstruction_holder` mirrors `subsystem_holder` and for the same reason. A
`Device` is destroyed every time the app is backgrounded, and a reconstruction
owned by it would throw away the mapped world on every press of Home. That is the
one thing in this app that has to survive, so it lives beside the `VkInstance`
rather than inside the renderer.

`MarkAllDirty` is the other half of that lifetime. After a `Device` rebuild every
node holding a chunk mesh has been destroyed while the voxels are still here, so
the renderer asks for the whole map back rather than assuming an empty scene means
an empty world.

### The types that cross the boundary

Nothing here mentions Eigen, and that is a build constraint rather than a style
preference. See *"The boundary, and why it is also a build setting"* above.

| type | what it is |
|---|---|
| `ChunkKey` | three `int32_t`. `Eigen::Vector3i` on the other side. |
| `ChunkUpdate` | key, world-space origin, vertex count, index count. Deliberately carries no geometry. |
| `Vertex` | 24 bytes, position and normal, `static_assert`ed. Laid out to match `graphics::PositionNormalVertex` exactly. |
| `Config` | everything that has to be decided before the first frame. |

`ChunkUpdate` is cheap on purpose. The caller reads it, sizes and maps a buffer,
then asks `WriteChunk` to interleave straight into that buffer. Handing out the
geometry here instead would mean a full extra copy of every remeshed chunk every
frame.

`WriteChunk` takes a destination rather than returning a span for the same
reason: it makes the conversion from OpenChisel's separate vertex and normal
arrays a single pass, landing in GPU-visible memory.

Vertices are **chunk local**, with the chunk's origin already subtracted, and the
node's transform is what puts them back in the world. That is not only tidiness.
A room is tens of metres across, and float32 near 30.0 has about two micrometres
of precision left after the exponent takes its share. Chunk-local coordinates keep
the sub-millimetre detail marching cubes worked to produce.

### Spatial hashing lives outside this namespace

CHISEL hashes chunks internally, but the renderer keeps its own map from chunk
coordinates to scene nodes. `graphics::SpaceChunk` is a component on `Node`
alongside `Renderable` and `Camera`, hashed with the same Teschner primes
`ChunkHasher` uses.

That map is a **cache**, not the source of truth, and the distinction is the
lifetime one above. Every entry in it is destroyed with the `Device`. The voxels
are not.

## The remesh cycle, and the starvation bug it exists to avoid

`Remesh(maxChunks)` runs marching cubes on a bounded number of dirty chunks per
frame and returns how many it did. Budgeting is not optional: integration marks
the whole 3x3x3 neighbourhood of every chunk it touches, because meshing reads
neighbours to close the seams between chunks, so sweeping a room dirties hundreds
at once.

The obvious implementation of that budget starves, and it starved here for long
enough to be blamed on three other things first.

```cpp
// WRONG
for (auto it = pending.begin(); it != pending.end() && done < maxChunks;) {
    const chisel::ChunkID id = it->first;
    it = pending.erase(it);
    ...
}
```

`ChunkSet` is `std::unordered_map<ChunkID, bool, ChunkHasher>`, so `begin()` is
the first non-empty **hash bucket**. That is a fixed and arbitrary function of the
chunk coordinates. Meanwhile integration re-inserts the same chunks every frame
into the same buckets, so the loop drains the same low-bucket chunks forever and
never reaches the high-bucket ones.

The symptom on the device was a backlog pinned at 129 chunks while 240 chunks a
second were being remeshed, and geometry that appeared in some regions and never
in others with no geometric pattern to it. An object could be cut cleanly in half
along a chunk boundary and stay that way indefinitely. **The budget was never the
constraint. The ordering was.**

What is there now is a cycle. Everything pending goes into a queue, the queue
drains at the budget over as many frames as it takes, and it refills only once
empty. Chunks dirtied mid-cycle collect in Chisel's set and are picked up at the
next refill, so every dirty chunk is meshed within `queueSize / maxChunks` frames.

`pendingRemeshCount()` reports the queue and the set together. Reporting only the
set would read as zero at the moment a full cycle had just been taken out of it.

## The far plane trap

`Chisel::IntegrateDepthScan` does **not** use the camera's configured far plane.
It calls `depthImage->GetStats` and builds its frustum from the maximum value in
the image, then allocates a chunk for every chunk that frustum meets.

Depth is uint16 millimetres, so a single stray pixel can claim 65 metres. Frustum
volume grows with the cube of its depth. One outlier turned a 5 metre view worth
roughly 190 chunks into hundreds of thousands, and at 32 KiB of voxels each the
process passed 1.1 GB and died inside `Chunk::AllocateDistVoxels`.

Two things in `Integrate` exist because of this, and neither is decoration:

- **The depth clamp.** Anything outside `[nearPlane, farPlane]` becomes NaN,
  exactly like a sample ARCore never had. This is what keeps the frustum finite.
- **The confidence gate runs first.** The wildest readings in a raw frame are the
  near-zero confidence ones. Letting them set the far plane and only then
  discarding their depth would pay the whole cost of the outlier and collect none
  of its geometry. Measured: with the gate in place, the furthest accepted sample
  in a room-scale sweep stays under 2.7 m against a 3.0 m far plane, so the far
  plane stops being the binding constraint at all.

Raw depth made this worse than the smoothed stream did, for a reason worth
remembering: smoothing bounds outliers, and sparsity does not.

### Why `farPlane` defaults to 3 m

It is the most expensive number in `Config`, and not because of quality. The
integrator visits every chunk whose box meets the frustum and projects all 4096
of its voxels whether or not any surface is near them, so per-frame cost tracks
frustum **volume**. Going from 5 m to 3 m leaves 22% of it.

Chosen from the scene rather than by tuning. This is a room-scale reconstruction,
ARCore's raw depth on interior surfaces is unreliable well before 5 m, and a wall
at 4 m contributes samples too coarse to mesh usefully at 4 cm voxels. Raising it
is a capability change rather than a knob.

## The empty-frame guard

`Integrate` returns early when no sample survived. This is a safety guard.

`GetStats` skips NaN, so on an all-NaN image it leaves its outputs at their
initial values of `FLT_MAX` and `-FLT_MAX` and hands those to `SetNearPlane` and
`SetFarPlane`. The frustum built from an inverted, infinite pair of planes has a
bounding box to match, and `GetChunkIDsIntersecting` then loops over the chunk
indices between two saturated integers.

Rare before the confidence gate existed, because a frame with no readings at all
is rare. Not rare after it: a dark room, a phone still on a desk, or the first
seconds after a resume all produce frames where everything is below threshold.

## Reading the instruments

Everything this namespace logs is tagged `RECONPROBE`, and that token appears
nowhere else on the system. Filtering by log tag alone is not enough in practice,
because ARCore is extremely chatty under its own tags.

The probes state verdicts rather than printing raw numbers for someone to
interpret, since every check is a fixed comparison against a fixed expectation
and having the device decide removes the step where the numbers get read wrong.

| line | what it settles |
|---|---|
| `VERDICT OK` / `VERDICT FAIL` | intrinsics rescaling. The single most consequential thing to get right, and see the FOV ratio check above for why it catches what individual numbers do not. |
| `confidence map PRESENT` / `ABSENT` | whether the confidence half of raw depth is actually arriving. |
| `first frame with data` | sample budget on a frame that actually has readings. Fires on the first such frame, not the first frame, because a stationary phone reports zero valid samples and a probe that speaks once would report 0% forever. |
| `integrate N ms mean over 60 frames` | where the frame went. The CPU cost was known and assumed to be here before this existed, and the far plane was about to be cut on that assumption. |
| `depth budget over the window` | how many samples ARCore never produced. Bounds what filtering can achieve. |
| `confidence of the samples that DID exist` | the bimodal distribution above. Decides whether any threshold is meaningful. |
| `chunk vertex counts ... in buckets of 512` | settles the mesh capacity in `graphics::HelloWorld`, which is fixed at creation. |
| `dedup ratio` | 1.00 means the edge keys are not matching and vertex sharing is silently not happening, which looks identical on screen. |

---

# Status

**Working, and validated on a device.** The whole chain runs: depth acquisition,
intrinsics rescaling through a measured crop, unprojection, TSDF integration,
marching cubes with shared vertices, chunk meshes uploaded to Vulkan and drawn
over the camera feed.

The validation sequence this section used to prescribe was followed and each step
earned its place. The centre-pixel marker caught nothing on its own, because a
live marker cancels its own error: reconstruction uses `sensorPose` and the
renderer uses `displayPose`, so a wrong pose looked correct until the marker was
**frozen** in world space and the phone moved away from it. Worth knowing if any
of this ever needs revalidating.

| | measured |
|---|---|
| frame loop | 30.0 fps, camera rate |
| integrate | 19 ms with 111 chunks in view |
| GPU frame | 0.82 ms |
| dedup ratio | 4.69x |
| mesh memory | 4 MiB across 64 chunk nodes |
| chunk capacity overflows | 0 of 204 remeshes at 1024 vertices |

**Known wrong, in the order worth fixing:**

1. **Between 40% and 60% of samples arrive with no estimate at all**, and no
   weighting scheme reaches a measurement that was never made. This is the
   largest remaining hole in coverage and the one lever left on it is the
   smoothed stream, discussed below.
2. **Truncation does not depend on distance.** `truncationQuadratic` and
   `truncationLinear` are both zero, so tau is a constant 0.06 m at every range,
   while ARCore documents its depth error as growing quadratically with distance.
   Dynamic truncation is one of the things CHISEL contributes over plain
   KinectFusion and it is currently switched off. It has never been evaluated on
   its own, only as part of a four-change experiment that was reverted whole.

**Not wrong, and not worth trying to fix:**

- The **sawtooth boundary** at the frontier of the reconstruction. Marching cubes
  refuses to mesh a chunk's outer voxel layer until `allNeighborsObserved`, so
  the edge of what has been seen always arrives as a zigzag. It resolves as the
  sweep continues.

**Next: the smoothed stream as hole fill, and why the earlier argument does not
forbid it.**

Raw depth was chosen over the smoothed stream because ARCore has already fused
the smoothed one temporally, so integrating it means averaging correlated data
and converging on the smoothing instead of on the surface. That argument holds
wherever raw depth exists. It says nothing about the 40% to 60% of pixels where
raw depth does not exist, because there the alternative to a smoothed guess is
not a better measurement, it is a hole.

So the shape is: raw at its confidence weight where it has an estimate, smoothed
at a low fixed weight only where raw is zero. The per-pixel weight image already
built for the confidence map is the mechanism, and it costs one more acquisition
rather than a session reconfiguration, since `AR_DEPTH_MODE_AUTOMATIC` exposes
both.

The reason this is the only lever worth pulling on coverage is the hardware.
This device has **no depth sensor**, and neither do most Android phones now:
Samsung dropped time of flight from its flagship line after the S20 Ultra era,
and ARCore's Depth API works without one by running motion stereo on the single
RGB camera. Depth here is computed, not measured. That is the direct cause of the
bimodal confidence, of untextured surfaces returning zero rather than a poor
value, and of the quadratic error growth. It is not a sensor that could be
better, it is the absence of a sensor, so nothing in this pipeline can raise the
ceiling. Only using more of what is already produced can.

**What the current quality unlocks.** The surface is now good enough that the
remaining noise can be attacked on the **mesh** rather than in the field.
Decimation or smoothing over chunk meshes is pure, wide, and fixed in output
size, which is the shape a compute shader wants, and it sits in the same pipeline
stage as the marching cubes migration identified above as the first good
candidate. That option did not exist while entire chunks were missing for
undiagnosed reasons: a smoothing pass would have produced a better looking
surface and hidden the starvation bug underneath it.
