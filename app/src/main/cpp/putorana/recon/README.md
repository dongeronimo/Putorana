# putorana::recon

Turning what the camera measures into a surface we can draw and collide against.

Nothing here yet but this document. That is deliberate: the arithmetic below has
to be right before any of it is worth writing down as code, and every one of the
mistakes it warns about produces a reconstruction that *looks* like a plausible
room and is wrong.

---

# The decision, and why

Two options were on the table: use OpenChisel, or write the algorithm ourselves.

**The plan is OpenChisel now, and moving pieces of it onto compute shaders over
time until there is nothing of it left.** That is not a compromise between the two
options — it is a sequence. The library buys a working reconstruction to look at
and argue with; each piece that turns out to matter gets replaced deliberately,
with the original still running beside it to check the replacement against. The
alternative was writing all of it before seeing any of it work, and being unable
to tell a maths bug from a plumbing bug when the result looked wrong.

OpenChisel is the reference implementation of the CHISEL paper (Klingensmith et
al., RSS 2015), written for Google Tango. It was measured before choosing: the
core library is **4814 lines across 16 .cpp files, depending only on Eigen and
the STL** — no ROS, no PCL, despite the catkin packaging. It compiles clean for
arm64-v8a under NDK 28 at `-std=c++17`, unmodified. It is MIT licensed.

## What was wrong with it, and what we did about it

The objections were real. They were also, every one of them, about 2014 C++
rather than about the algorithm — which is why forking beat rejecting:

- **Its memory layout was wrong for a phone.** `DistVoxel` had a *virtual
  destructor*, so on arm64 every voxel carried an 8-byte vtable pointer next to
  its 4-byte SDF and 4-byte weight: 16 bytes where 8 would do. `ColorVoxel` was
  the same — 16 bytes to store 4 of RGBA. A voxel cost 32 bytes instead of 12,
  and half of every cache line the integrator touched was vtable pointer. Nothing
  was ever used polymorphically through those pointers: the entire codebase
  contains exactly **three** inheritance relationships.
- **`Truncator` and `Weighter` dispatched virtually from inside the per-voxel
  loop.** Their bodies are a handful of arithmetic operations, so the cost was
  not the dispatch but the inlining, hoisting and vectorisation it prevented.
  `QuadraticTruncator` computed its square as `pow(reading, 2)`, which behind a
  virtual boundary could not be folded — squaring a float was a libm call, per
  voxel.

Both are fixed in the fork at `third_party/open_chisel/`, whose README documents
every change and the measurements behind them. A voxel is now **12 bytes**, the
library emits **no vtables at all**, and the compiled output contains no calls to
`pow`.

What is still wrong with it is listed there too, deliberately unfixed: `size_t`
mesh indices feeding a `VK_INDEX_TYPE_UINT32` buffer, and Eigen where the rest of
this project is GLM.

`Threading.h::parallel_for` is *not* on that list, despite appearances. Its
`threshold = 1000` default means it spawns no threads at all below ~1000 chunks
in the frustum, which is where we operate — so the integrator is single-threaded
today whether or not anyone intended it. The fork's README has the arithmetic.
Worth knowing mainly so nobody spends an afternoon tuning a thread count that
does nothing.

## The boundary, and why it is also a build setting

Eigen is an expression-template library: unoptimised, every vector operation
becomes a chain of real function calls instead of collapsing into a few
instructions, and `eigen_assert` bounds-checks each access. A debug build of
anything Eigen-heavy is not a bit slower, it is *categorically* slower — slow
enough that you stop being able to tell a performance bug from the debug build,
which is how afternoons disappear.

On Android we can do something about that which Windows does not allow: build
this one library with full optimisation while the app around it stays debuggable.

The reason the Windows intuition does not transfer is specific. There, debug and
release are **different C runtimes** — different heaps, so memory allocated in one
and freed in the other corrupts; and `_ITERATOR_DEBUG_LEVEL` changes the *layout*
of standard containers, so a `std::vector` is not even the same type across the
boundary. Both are ABI breaks. Android has one libc++ and one bionic allocator
for the entire process, and optimisation level is not an ABI property. Mixing
`-O0` and `-O3` translation units is ordinary there.

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
be destroyed every time the user presses Home — throwing away the reconstructed
world, which is the one thing in this app that must survive. The workarounds are
to re-upload the whole volume on every resume, or to keep a CPU mirror; and if
there is a CPU mirror, the CPU version is already written.

`ar::Subsystem` solved exactly this problem already, and correctly: it lives at
process scope because destroying the session throws away tracking. The voxel grid
has the same property and belongs at the same level.

The second reason is arithmetic. ARCore's depth map is small — **measured at
160x90 on the device this was developed against**, up to 640x480 on some others.
That is 14,400 samples per frame; at 20-40 voxels touched per ray, a few hundred
thousand voxel updates. That is milliseconds on one arm64 core. The expensive part of a GPU implementation is not the
integration kernel — it is dynamic chunk allocation on the GPU (a hash table with
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

## Which pose — the trap this section exists for

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
ARCore says so in as many words — `ArCamera_getImageIntrinsics` returns "the
unrotated and uncropped intrinsics for the image (CPU) stream", and the
`AR_COORDINATES_2D_IMAGE_*` spaces are documented as the CPU image while the
`VIEW` ones are explicitly "display-rotated". Pixels that were never rotated must
be unprojected with a pose that was never rotated either.

### This is not a bug in the render path

Worth stating plainly, because the natural question is whether `graphics::ArCamera`
should have been using the intrinsics all along. It should not, and nothing there
needs fixing.

The two halves of the app ask **inverse questions**:

- Rendering asks *"where on screen does this 3D point land?"* — 3D → 2D. ARCore
  answers it with a finished `projection` matrix. The intrinsics are already
  inside that matrix; that is what a projection matrix *is*. They are absent from
  the render path because they are already there, in matrix form.
- Reconstruction asks the opposite: *"this pixel reads 2.3m — where in 3D is
  that?"* — 2D → 3D. That is the inverse of a projection, it cannot be done with
  a projection matrix that has already discarded depth, and it operates on a
  **different image** (the depth map, unrotated) under a **different rotation
  convention**.

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
shows why — but code written for a time-of-flight sensor, which usually reports
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

### Automatic vs raw, and the confidence image we are not reading — OPEN

There are two depth streams, and we currently read one of them.

| | **Automatic** (what we read) | **Raw** |
|---|---|---|
| call | `ArFrame_acquireDepthImage16Bits` | `ArFrame_acquireRawDepthImage16Bits` |
| coverage | dense — every pixel has a value | sparse — 0 where ARCore is unsure |
| filtering | smoothed, temporally fused by ARCore | "mostly unfiltered" |
| confidence | none | `ArFrame_acquireRawDepthConfidenceImage` |

Confidence is not a property of a depth sample that can be asked for separately.
It belongs to a **different image**, and that image pairs with raw depth. So there
is no confidence value for the map we read today.

Both streams are already available to us: the header states that "raw depth data
is also available when `AR_DEPTH_MODE_AUTOMATIC` is selected", which is the mode
`Subsystem::Create` sets. Reading raw as well costs an acquisition, not a
reconfiguration.

**The open question is which one a TSDF should actually integrate**, and there is
a real argument on each side:

- *For raw.* A TSDF performs its own temporal fusion. Integrating a map ARCore
  has already fused temporally means fusing the same measurement twice and coming
  out over-confident about it — the weights claim more independent evidence than
  was ever collected. And ARCore's own guidance for raw depth is that it is
  "intended to be used in cases that involve understanding of the geometry in the
  environment", which is a description of this namespace.
- *For automatic.* Dense data is what makes a first reconstruction visibly work
  or visibly not, and sparse input makes an incomplete surface ambiguous between
  "the algorithm is wrong" and "there was no data there".

Automatic is the current choice for the second reason alone: it gets us to
something on screen. That is a starting position, not a conclusion. Revisit it
with measurements on a real device — not with taste.

---

# The mathematics

## Intrinsics

Four numbers describe the entire 3D-to-2D map of a pinhole camera. For a point
`(X, Y, Z)` in camera space, with `Z` measured along the principal axis:

```
u = fx · (X / Z) + cx
v = fy · (Y / Z) + cy
```

`fx`, `fy` are the focal length expressed **in pixels** — in pixel widths and
pixel heights respectively. They differ only when the sensor's pixels are not
square, which on a phone they essentially always are, so expect `fx ≈ fy` and
treat a large gap as a bug rather than as a property of the hardware.

`cx`, `cy` are the **principal point**: where the optical axis actually pierces
the sensor. Near the image centre, never exactly at it, and that small offset is
why ARCore's projection matrix carries terms outside the diagonal — see the
comment in `graphics/ArCamera.cpp` about negating the whole row rather than just
`[1][1]`.

A useful sanity check, since `fx` in pixels is hard to have intuition about:

```
horizontal FOV = 2 · atan(width / (2 · fx))
```

A phone's main camera should come out somewhere around 60-70°. If that number is
absurd, the intrinsics do not belong to the image they are being used with.

### Rescaling: a crop, then a scale — MEASURED

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
the field of view of the image you scaled *from* — so the maths ends up believing
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
that crops the other way — or not at all — falls out of the same three lines.

Two things in there are easy to get wrong:

- **`cx` and `cy` are positions, not lengths.** The crop origin has to come off
  them *before* the scale. This is invisible on a perfectly centred sensor,
  because `cx − cropLeft` lands back in the middle anyway — and no real sensor is
  perfectly centred.
- **`s` is one number.** Because the crop restores the aspect ratio before the
  resample, the horizontal and vertical scales are equal. That is the invariant
  worth asserting: if they ever differ, the crop is not centred and this model is
  wrong for that device.

On the measured numbers: `cropTop = 60`, `s = 0.25`, `fx 439.73 → 109.93`,
`fy 440.19 → 110.05`, `cx 314.88 → 78.72`, `cy 241.35 → 45.34`. The sensor's
off-centre principal point (+1.35 px of 480) survives as +0.34 px of 90 — exactly
a quarter — which is independent confirmation that the crop really is centred.

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
**1.335 against a 1.778 image** — a 25% error, on a frame where the horizontal
FOV, the stride, and the principal point all read as fine.

`Subsystem` computes it on the first depth frame and states the verdict itself.
Search logcat for `RECONPROBE`; the last line is `VERDICT OK` or `VERDICT FAIL`.
It is the one number to look at when a reconstruction comes out subtly the wrong
shape.

## Unprojection

The operation this whole namespace is built on: pixel + depth → a 3D point in the
world.

### Step 1 — pixel to camera space

Invert the projection. Given pixel `(u, v)` and depth `d` metres, and knowing
`Z = d` because the depth is along the principal axis:

```
X = (u - cx) · d / fx
Y = (v - cy) · d / fy
Z = d
```

**This is where Z-versus-range earns its keep.** The ray through pixel `(u,v)` has
direction `((u-cx)/fx, (v-cy)/fy, 1)` — note the third component is exactly `1`,
un-normalised. Because `d` is measured along Z, scaling that un-normalised
direction by `d` lands on the point directly. Had ARCore given range instead, the
direction would first have to be normalised — a square root per pixel, and a
different answer.

### Step 2 — into ARCore's axis convention

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

### Step 3 — camera space to world space

`sensorPose` is a rotation and a translation. With `q` its unit quaternion and `t`
its translation:

```
P_world = q · P_cam · q⁻¹ + t
```

In GLM, `glm::quat(w, x, y, z) * P_cam + t`. ARCore's raw order is `qx, qy, qz,
qw` and glm's constructor takes **w first** — the same reordering already
commented in `graphics/ArCamera.cpp`, and the same trap. Getting it wrong yields
a rotation that looks plausible and that no single still frame reveals as wrong.

### The check to run first

Take the centre pixel, `u = cx`, `v = cy`. Then `X_cam = Y_cam = 0` and
`Z_cam = -d`: a point straight down the optical axis at distance `d`. Transform it
by `sensorPose` and it should sit exactly where the phone is pointing, `d` metres
away. That single point, drawn as a marker, is the first thing worth building —
before any voxel exists.

## Rotation, and why it never appears in this namespace

The obvious question, having read the above: don't the intrinsics have to be
rotated to match the display at some point — if not now, then when the mesh is
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
how the depth pixels are indexed **and** switching to `displayPose` — three
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
pixel — tap-to-place, or occluding virtual content against real depth. That is
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
thousand frames are not a better surface — they are a thousand noisy opinions
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
only a convention — flipping it flips every normal in the output mesh — but it has
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
new data arrives. Topology resolves itself — no stitching, no overlap tests,
nothing.

## One ray, with numbers

Voxels of 4cm, truncation τ = 12cm. A depth pixel reads **d = 2.00m**. Walk the
ray and store `sdf = d − t` at each voxel `t` metres along it:

| t (m) | sdf = d − t | meaning |
|---|---|---|
| 1.84 | — | outside the band, untouched |
| 1.88 | +0.12 | band edge, free space |
| 1.92 | +0.08 | free space |
| 1.96 | +0.04 | free space, closing in |
| **2.00** | **0.00** | **the surface** |
| 2.04 | −0.04 | behind the surface |
| 2.08 | −0.08 | behind |
| 2.12 | −0.12 | band edge |
| 2.16 | — | outside, untouched |

## The fusion rule

The next frame measures the same wall at **2.02m** — the same wall, a different
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
magnitude — interpolating where the surface crosses a marching-cubes edge,
gradients, clearance queries — would break.

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
above the back — an observation of free space being more trustworthy than a guess
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
bounding box declared up front — "reconstruct this 4×4×4m cube" — and allocate
all of it, air included.

CHISEL's contributions are about making that survive on a phone, in a scene with
no declared size:

- **spatial hashing of chunks** — small blocks of voxels allocated only where
  surface actually appears, so empty space costs nothing at all, not even a hash
  entry, and there is no bounding box to declare in advance;
- **dynamic truncation** — τ grows with distance, because depth error grows with
  distance (quadratically, for ARCore, which is where `QuadraticTruncator` comes
  from);
- **space carving** — actively clearing voxels observed to be empty, so that
  something which moves away does not leave a ghost behind.

So when the paper concentrates on hashing and chunk management rather than on the
distance field itself: the field is the inherited part, and the storage is the
new part. The storage is also the part this namespace has to get right, since it
is the reason we are writing it ourselves rather than taking OpenChisel whole.

---

# Status

**Done** — the input is available and the app builds:

- Depth enabled on the session, guarded by `ArSession_isDepthModeSupported`, so a
  device without depth support still gets a working camera feed and tracking
  rather than a failed `ArSession_configure`.
- `CameraFrame::depth` — the 16-bit map, borrowed for one frame like the camera
  image.
- `CameraFrame::sensorPose` alongside the renamed `displayPose`.
- `CameraFrame::imageIntrinsics`, and `depth.intrinsics` rescaled to the depth
  map's own resolution.
- First-frame logging of both resolutions, both aspect ratios and both sets of
  intrinsics.

**Next, and in this order:**

1. **Run it on a device and read the log line.** Confirm the depth resolution,
   that the aspect ratios agree (no crop), and that the FOV implied by `fx` is
   sane. Everything downstream is built on those three numbers.
2. **Unproject one pixel and draw it.** The centre-pixel check above, as a marker
   in the world. This validates the entire chain — units, both sign flips,
   quaternion order, pose choice — with geometry simple enough that being wrong
   is obvious.
3. **Unproject the whole map as a point cloud.** Still no voxels. A wrong `fy`,
   a crop, or a transposed row stride are all invisible on one point and
   unmistakable on twenty thousand.
4. Only then: chunks, TSDF integration, marching cubes.
