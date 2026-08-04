# OpenChisel, vendored fork

The TSDF library from Klingensmith et al., *CHISEL: Real Time Large Scale 3D
Reconstruction Onboard a Mobile Device using Spatially Hashed Signed Distance
Fields* (RSS 2015). It is the reference implementation of the paper that
`putorana::recon/README.md` explains.

    https://github.com/personalrobotics/OpenChisel
    commit 3a8f2fee985eea44c0bd5c3007af464e185f577b (25 Feb 2024)

Only the `open_chisel/` package is taken. The sibling `chisel_ros/` package is a
ROS node and is not vendored.

MIT, © 2014 Matthew Klingensmith and Ivan Dryanovski. Upstream has no top-level
`LICENSE` file. The licence lives in a banner at the head of every source file,
and those banners are left intact.

## Why a fork rather than a dependency

It is not maintained as a library: there are no releases, no versioning, and the
build is a catkin package that assumes a ROS workspace. Consuming it unmodified
would mean either dragging catkin into an Android build or maintaining a patch
set anyway. And the changes below are exactly the kind that cannot be made from
outside a library, because they alter the layout of its types.

Every change is annotated in place with a `LOCAL MODIFICATION:` comment
explaining what it was and why it changed, so the reasoning survives even if this
file is not read. The first two changes below are layout and dispatch work done
before anything had run. The three after them came out of running it on a device,
and each one is there because the reconstruction was visibly wrong without it.

## What was changed, and what it bought

### 1. Vestigial virtual destructors removed (16 classes)

Upstream declares `virtual ~X()` on nearly every class. The whole codebase
contains exactly **three** inheritance relationships, all of them in the
Truncator/Weighter policy hierarchy dealt with below, so on the other sixteen
classes the vtable pointer was paid for and never used.

For most of those sixteen the cost is nothing: they are single heap objects, and
8 bytes is noise. It matters enormously for two of them, because those two live
in arrays of millions:

| | upstream | here |
|---|---|---|
| `sizeof(DistVoxel)` | 16 | **8** |
| `sizeof(ColorVoxel)` | 16 | **4** |
| bytes per voxel | 32 | **12** |
| a 16³ chunk (dist + colour) | 128 KB | **48 KB** |

`DistVoxel` is two floats. Upstream spent 8 bytes of vtable pointer to carry 8
bytes of payload, so half of every cache line the integration loop touched was
vptr. `ColorVoxel` is four `uint8_t`, so 8 bytes of vptr for 4 bytes of colour,
padded out to 16.

At a working resolution this is the difference between a reconstruction that fits
in a phone's memory budget and one that does not; put another way, **it buys back
roughly one notch of voxel resolution.**

For these two classes the destructor is deleted outright rather than merely
un-`virtual`ed, which additionally makes them trivially destructible, so tearing
down a `std::vector<DistVoxel>` becomes a no-op instead of a loop of empty calls
over millions of elements.

The library now emits **no vtables at all**.

### 2. `Truncator` and `Weighter` collapsed into concrete value types

This was worth more than the destructors, and it is not what the memory table
above measures.

`ProjectionIntegrator::Integrate` calls both of these **inside the per-voxel
loop**:

```cpp
float truncation = truncator->GetTruncationDistance(depth);
...
voxel.Integrate(surfaceDist, weighter->GetWeight(surfaceDist, truncation));
```

A 16³ chunk is 4096 voxels, and every chunk in the frustum is swept every frame,
so those two virtual calls ran millions of times per second on function bodies
that are a handful of arithmetic operations. The dispatch was the smaller cost.
The real loss was that the compiler could not inline the bodies into the loop,
could not hoist the coefficient loads out of it, and could not vectorise across
voxels.

Worse, `QuadraticTruncator` computed its square as `pow(reading, 2)`. Behind a
virtual boundary that call could not be folded away, so squaring a float was an
actual `libm` call, per voxel.

The fix rests on an observation: the quadratic truncation model *subsumes* the
constant one, since a constant truncation is just `a = b = 0`. So one concrete
class covers both with no loss of configurability, since the truncation model
stays a runtime parameter and only the dispatch disappears. `Weighter` had
exactly one implementation in the entire codebase, so the vtable there was
carrying a single divide.

  * `Truncator` is now concrete: `τ(d) = |a·d² + b·d + c| · s`, in Horner's form,
    with a `Truncator::Constant(τ)` factory for the degenerate case.
  * `Weighter` is now concrete: `w = weight / (2τ)`.
  * `ConstantTruncator.h`, `QuadraticTruncator.h` and `ConstantWeighter.h` are
    deleted.
  * `ProjectionIntegrator` holds both **by value** instead of by `shared_ptr`,
    which also removes a pointer chase from the hot path.

The compiled library contains no calls to `pow`.

### 3. Marching cubes shares vertices along topological edges

`MarchingCubes.h`, `MarchingCubes.cpp`, `ChunkManager.cpp`.

Upstream emits a triangle soup. Every triangle allocates three fresh vertices,
so a vertex sitting where six triangles meet is stored six times with six
identical normals, and the index buffer carries no information beyond `0, 1, 2,
3, ...`. Upstream even asserts the property: `vertices.size() == indices.size()`.

The fix keys each vertex on the **grid edge** it was interpolated on rather than
on its position. A cube corner offset table plus the existing `edgeIndexPairs`
gives the axis and the lower endpoint of any edge, which packs into a 64-bit key:

```cpp
static inline uint64_t EdgeKeyFor(const Eigen::Vector3i& cubeIndex, int edge);
```

Keying on topology rather than on coordinates matters. A position-based key needs
an epsilon, and an epsilon on interpolated floats either merges vertices that
should be distinct or fails to merge ones that should not be. Two cubes sharing
an edge produce byte-identical keys with no tolerance involved.

`ChunkManager::GenerateMesh` owns the map for the duration of one chunk and
threads it through both the interior and border passes, then normalises the
accumulated normals. Normals are accumulated **unnormalised** during meshing so
that face contributions are area weighted, which is the standard result for
shared-vertex normals and comes out for free.

Measured on device: **4.08x to 4.69x** fewer vertices for the same geometry. A
soup reads exactly 1.00 and a fully shared manifold surface reads about 6, since
`V ≈ F/2` there. Mesh time went from 0.49 ms to 0.43 ms in the same test, and
mesh memory for a room dropped from 17 MiB across 263 nodes to 4 MiB across 64.

The upstream assert is replaced rather than deleted, since it was encoding a real
invariant that simply changed shape:

```cpp
assert(indices.size() % 3 == 0);
assert(vertices.size() <= indices.size());
```

### 4. Per-pixel integration weights

`ProjectionIntegrator.h`.

The depth path, `ProjectionIntegrator::Integrate`, passed a hardcoded weight:

```cpp
voxel.Integrate(surfaceDist, 1.0f);
```

The `weighter` member is consulted only from `IntegrateColor`. Any application
using the depth-only path (which is this one) had its weighting policy silently
ignored, and a configured `Weighter` did nothing at all.

That is fixable in place by calling the weighter, but it does not go far enough
for this application. ARCore ships a **confidence map** alongside raw depth, and
its samples are not of equal quality: a reading from a textured surface and a
reading from a blank painted wall arrive in the same image and deserve very
different votes. The weighter's signature, `GetWeight(surfaceDist, truncation)`,
has nowhere to put that.

So the integrator takes an optional image of per-pixel weights:

```cpp
inline void SetWeights(const std::shared_ptr<const DepthImage<float> >& w);
inline void ClearWeights();
```

Null restores upstream behaviour exactly, which is a weight of 1.0 everywhere. A
weight of 0 means do not fuse this sample, which lets a caller reject a pixel
without also blanking its depth, and that distinction is load bearing because the
non-NaN extent of the depth map is what sizes the frustum.

It is indexed at the same row and column the depth was read from, so it costs one
array read and no additional projection.

Why weighting rather than filtering the depth map upstream of the library: a hard
threshold makes the reconstruction honest and full of holes. A floor is exactly
the low-texture surface a confidence map is most pessimistic about, so any
threshold high enough to suppress bad geometry deletes the good geometry in the
same region. Measured at Google's recommended threshold of 128, 79% of samples in
a frame were rejected and a tiled floor came back as disconnected islands.
`putorana/recon/README.md` has the numbers.

### 5. Mutable access to the dirty set

`Chisel.h`, `GetMutableMeshesToUpdate()`.

Upstream's only way to remesh is `UpdateMeshes()`, which does every dirty chunk
and then clears the set. Integration marks the whole 3x3x3 neighbourhood of every
chunk it touches, so sweeping a room dirties hundreds at once and meshing all of
them inside one frame is a dropped frame.

With mutable access a caller can take a bounded number per frame and leave the
rest marked. `putorana::recon::Reconstruction::Remesh` does that, and its
`README.md` documents the starvation bug that the obvious implementation of it
contains.

### 6. Space carving rebuilt, because upstream's cannot remove anything

`DistVoxel.h`, `ProjectionIntegrator.h`.

Space carving is one of the things the CHISEL paper contributes over plain
KinectFusion. In the reference implementation it cannot delete a surface, for
four independent reasons. Any one of them alone is enough.

```cpp
inline void Carve() { Integrate(0.0, 1.5); }
```

**It adds weight to the voxel it is erasing.** `Integrate` accumulates, so a
voxel at weight 200 moves by 1.5/201.5, under one percent, and comes out heavier
than it went in. Every attempt makes the next one weaker.

**It pulls the SDF toward zero, which is the surface, not free space.** Combined
with the eligibility test below, which only admits voxels already at or behind a
surface, carving walks them asymptotically up to 0 from underneath and the sign
never flips. Marching cubes keys on sign changes, so the triangle stays.

```cpp
if (voxel.GetWeight() > 0 && voxel.GetSDF() < 1e-5)
```

**That test is the wrong half of the population.** A voxel just in front of an
object that has been taken away carries a small positive SDF and never qualifies,
so the near face of anything removed is structurally uncarvable.

**And the branch is unreachable at the configured distance.** Carving is an
`else if` after a test on `truncation + diag`, where `diag` is `2*sqrt(3)*res`.
At 4 cm voxels that is 0.1386 against a configured `carvingDist` of 0.05, so the
parameter is inert and carving silently begins wherever the integration band
happens to end. Raising `carvingDist` could only push the start further out.

What runs here instead:

  * `Carve(decay, minWeight)` **decays** the accumulated weight and `Reset`s the
    voxel to unobserved once it falls below the floor. Observing free space is
    evidence against what a voxel holds, not a competing measurement of it.
    `ChunkManager` treats weight below `1e-15` as unobserved and skips the whole
    cube, so that is what actually deletes geometry.
  * The SDF test is gone. The weight test stays, since an unobserved voxel has
    nothing to carve.
  * `diag` is **one** voxel diagonal rather than two. Half a diagonal is what the
    geometry justifies, since the test is on the voxel centre, but it was
    measured too small on the device: marching cubes needs all eight corners of a
    cube observed and the surface came back full of holes. One diagonal holds.
  * The carve test is `truncation + diag + carvingDist`, so `carvingDist` is a
    **dead zone above the integration band** instead of a threshold competing
    with it. Sharing an exact boundary made voxels on it flip between integrated
    and carved as depth noise crossed the line, which was visible as geometry
    flickering with nothing moving. The gap makes that impossible in one frame,
    and makes the parameter monotone in the direction its name implies.

`DistVoxel::Integrate` also takes an optional weight ceiling, defaulting to 0
which is upstream's unbounded running mean. Unbounded means a voxel observed
three hundred times cannot be corrected by fresh evidence at all, which is the
other half of why nothing ever went away.

## Verified

All 16 translation units compile clean, zero errors and zero failures, for
`aarch64-linux-android33`, NDK 28.2.13676358, clang `-std=c++17 -O2`. Both before
and after the changes above; the refactor did not fix a build problem, because
there was not one. The sizes in the table were measured on that target, not
assumed.

It has since been run. The library reconstructs a room on a Samsung SM-S731B at
30 fps with integration taking 19 ms for 111 chunks in view, and the dedup ratio
above is a device measurement rather than a prediction.

## This library is always built optimised

Even in Debug. `-O3 -DNDEBUG`, while the application around it keeps `-g` and no
optimisation, verified in the same build:

| translation unit | flags |
|---|---|
| `open_chisel/src/ProjectionIntegrator.cpp` | `-O3 -DNDEBUG -g` |
| `putorana/graphics/ArCamera.cpp` | `-g` |

Eigen is an expression-template library and is not merely slower unoptimised, it
is categorically slower; a debug build of the integrator is slow enough to
disguise real performance problems.

The full reasoning, meaning why this is safe on Android and not on Windows and
why the `NDEBUG` half of it forces `putorana::recon` to be a facade that never
leaks an Eigen type outward, is in `CMakeLists.txt` next to the code that does
it, and in `putorana/recon/README.md` under *"The boundary, and why it is also
a build setting"*.

The short version: **anything that includes `<open_chisel/...>` must also link
`open_chisel_fast_flags`**, or it will parse Eigen's headers with a different
`NDEBUG` than this library did and quietly violate the ODR.

## Known upstream problems left alone

Deliberately not fixed, so that the diff stays reviewable. The first two are
behavioural and cost real time on the device, so they are listed first and are
the next candidates to change. The rest are small.

  * **`Chisel.h::IntegrateDepthScan` ignores the camera's configured far plane.**
    It calls `depthImage->GetStats` and builds its frustum from the **maximum
    value in the image**, then allocates a chunk for every chunk that frustum
    meets:

        cameraCopy.SetNearPlane(minimum);
        cameraCopy.SetFarPlane(maximum);

    Depth is uint16 millimetres, so one stray pixel can claim 65 metres. Frustum
    volume grows with the cube of its depth, so a single outlier turned a 5 metre
    view worth roughly 190 chunks into hundreds of thousands. At 32 KiB of voxels
    each the process passed 1.1 GB and died inside `Chunk::AllocateDistVoxels`.

    Worked around rather than fixed: `putorana::recon` clamps depth to a
    configured range during its millimetre-to-metre conversion, so no
    out-of-range sample ever reaches `GetStats`. Anyone touching that conversion
    loop needs to know why the clamp is there.

    Note the related consequence: `minimum` also comes from the image, so the
    near plane moves with the data too.

Small, and worth revisiting once there is something running to measure:

  * **`threading/Threading.h`** looks alarming and is inert. Read carefully
    before spending time on it:

        group = max(max(1, threshold), N / nthreads);   // = max(1000, N/16)
        for (it = first; it < last - group; ...)        // no iterations if group >= N

    The `nthreads = 16` default is not what gates this. `threshold = 1000` is.
    Below ~1000 items `last - group` lands at or before `first`, the loop never
    runs, and everything executes on the calling thread. **You need >16000 items
    before all 16 threads appear.** Measured partitioning:

    | items | threads spawned |
    |---|---|
    | ≤ 1000 | **0** |
    | 1001 – 2000 | 1 |
    | 5000 | 4 |
    | 16001 | 16 |

    The items are chunks intersecting the view frustum, a few hundred at 4 cm
    voxels, order 1000 at 2 cm. So today this spawns no threads at all, and at
    fine resolution it would sit *on* the cliff, silently alternating between 0
    and 1 depending on where the camera points.

    There is also exactly one live call site (`Chisel.h`,
    `IntegrateDepthScanColor`); the depth-only path we would actually use, and
    the mesh update path, already have it commented out upstream. And the live
    one takes a mutex immediately after fanning out, so it is largely serialised
    regardless.

    Left alone deliberately. Not "threading to fix later". There is no threading
    here to fix. If integration ever measures too slow, the levers in order are:
    integrate at 5–10 Hz instead of per frame (depth is highly redundant
    frame-to-frame, and this is free), then voxel size, then marching cubes,
    which is the part that will actually hurt. Real parallelism belongs after all
    of those, and would need a persistent pool with atomic work claiming rather
    than a bigger `nthreads`: phone cores are asymmetric, so equal static blocks
    leave the big cores idling behind the little ones no matter how the count is
    chosen.
  * **`typedef size_t VertIndex`** in `mesh/Mesh.h`, 8 bytes per index, feeding
    a Vulkan index buffer that wants `VK_INDEX_TYPE_UINT32`. Costs a conversion
    pass on every mesh upload.
  * **`shared_ptr<Chunk>`** in the chunk hash map, so chunk lookups touch an
    atomic refcount.
  * **Eigen throughout**, where the rest of this project uses GLM. The surface is
    small enough to port (see the note in `CMakeLists.txt`), but there is no
    reason to until the extra dependency actually hurts.

## Updating from upstream

There is no meaningful upstream cadence to follow. The repository sees a commit
every year or two. If you ever do need to re-sync, the changes here are
mechanical enough to reapply by hand: grep for `LOCAL MODIFICATION:` to find all
of them.
