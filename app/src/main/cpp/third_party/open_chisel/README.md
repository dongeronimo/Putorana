# OpenChisel — vendored fork

The TSDF library from Klingensmith et al., *CHISEL: Real Time Large Scale 3D
Reconstruction Onboard a Mobile Device using Spatially Hashed Signed Distance
Fields* (RSS 2015). It is the reference implementation of the paper that
`putorana::recon/README.md` explains.

    https://github.com/personalrobotics/OpenChisel
    commit 3a8f2fee985eea44c0bd5c3007af464e185f577b (25 Feb 2024)

Only the `open_chisel/` package is taken. The sibling `chisel_ros/` package is a
ROS node and is not vendored.

MIT, © 2014 Matthew Klingensmith and Ivan Dryanovski. Upstream has no top-level
`LICENSE` file — the licence lives in a banner at the head of every source file,
and those banners are left intact.

## Why a fork rather than a dependency

It is not maintained as a library: there are no releases, no versioning, and the
build is a catkin package that assumes a ROS workspace. Consuming it unmodified
would mean either dragging catkin into an Android build or maintaining a patch
set anyway. And the changes below are exactly the kind that cannot be made from
outside a library, because they alter the layout of its types.

The changes are small — 26 files, +146/−248 — and every one of them is annotated
in place with a `LOCAL MODIFICATION:` comment explaining what it was and why it
changed, so the reasoning survives even if this file is not read.

## What was changed, and what it bought

### 1. Vestigial virtual destructors removed (16 classes)

Upstream declares `virtual ~X()` on nearly every class. The whole codebase
contains exactly **three** inheritance relationships, all of them in the
Truncator/Weighter policy hierarchy dealt with below — so on the other sixteen
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
vptr. `ColorVoxel` is four `uint8_t` — 8 bytes of vptr for 4 bytes of colour,
padded out to 16.

At a working resolution this is the difference between a reconstruction that fits
in a phone's memory budget and one that does not; put another way, **it buys back
roughly one notch of voxel resolution.**

For these two classes the destructor is deleted outright rather than merely
un-`virtual`ed, which additionally makes them trivially destructible — tearing
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

Worse, `QuadraticTruncator` computed its square as `pow(reading, 2)` — behind a
virtual boundary that call could not be folded away, so squaring a float was an
actual `libm` call, per voxel.

The fix rests on an observation: the quadratic truncation model *subsumes* the
constant one, since a constant truncation is just `a = b = 0`. So one concrete
class covers both with no loss of configurability — the truncation model stays a
runtime parameter, only the dispatch disappears. `Weighter` had exactly one
implementation in the entire codebase, so the vtable there was carrying a single
divide.

  * `Truncator` is now concrete: `τ(d) = |a·d² + b·d + c| · s`, in Horner's form,
    with a `Truncator::Constant(τ)` factory for the degenerate case.
  * `Weighter` is now concrete: `w = weight / (2τ)`.
  * `ConstantTruncator.h`, `QuadraticTruncator.h` and `ConstantWeighter.h` are
    deleted.
  * `ProjectionIntegrator` holds both **by value** instead of by `shared_ptr`,
    which also removes a pointer chase from the hot path.

The compiled library contains no calls to `pow`.

## Verified

All 16 translation units compile clean — zero errors, zero failures — for
`aarch64-linux-android33`, NDK 28.2.13676358, clang `-std=c++17 -O2`. Both before
and after the changes above; the refactor did not fix a build problem, because
there was not one. The sizes in the table were measured on that target, not
assumed.

**Not verified:** none of this has been run. It compiles and it is the right
shape; whether it reconstructs anything is an open question, and the first real
test is still the on-device one described in `putorana::recon/README.md`.

## This library is always built optimised

Even in Debug. `-O3 -DNDEBUG`, while the application around it keeps `-g` and no
optimisation — verified in the same build:

| translation unit | flags |
|---|---|
| `open_chisel/src/ProjectionIntegrator.cpp` | `-O3 -DNDEBUG -g` |
| `putorana/graphics/ArCamera.cpp` | `-g` |

Eigen is an expression-template library and is not merely slower unoptimised, it
is categorically slower; a debug build of the integrator is slow enough to
disguise real performance problems.

The full reasoning — why this is safe on Android and not on Windows, and why the
`NDEBUG` half of it forces `putorana::recon` to be a facade that never leaks an
Eigen type outward — is in `CMakeLists.txt` next to the code that does it, and in
`putorana/recon/README.md` under *"The boundary, and why it is also a build
setting"*.

The short version: **anything that includes `<open_chisel/...>` must also link
`open_chisel_fast_flags`**, or it will parse Eigen's headers with a different
`NDEBUG` than this library did and quietly violate the ODR.

## Known upstream problems left alone

Deliberately not fixed, so that the diff stays reviewable. Each is small, and
each is worth revisiting once there is something running to measure:

  * **`threading/Threading.h`** looks alarming and is inert. Read carefully
    before spending time on it:

        group = max(max(1, threshold), N / nthreads);   // = max(1000, N/16)
        for (it = first; it < last - group; ...)        // no iterations if group >= N

    The `nthreads = 16` default is not what gates this — `threshold = 1000` is.
    Below ~1000 items `last - group` lands at or before `first`, the loop never
    runs, and everything executes on the calling thread. **You need >16000 items
    before all 16 threads appear.** Measured partitioning:

    | items | threads spawned |
    |---|---|
    | ≤ 1000 | **0** |
    | 1001 – 2000 | 1 |
    | 5000 | 4 |
    | 16001 | 16 |

    The items are chunks intersecting the view frustum — a few hundred at 4 cm
    voxels, order 1000 at 2 cm. So today this spawns no threads at all, and at
    fine resolution it would sit *on* the cliff, silently alternating between 0
    and 1 depending on where the camera points.

    There is also exactly one live call site (`Chisel.h`,
    `IntegrateDepthScanColor`); the depth-only path we would actually use, and
    the mesh update path, already have it commented out upstream. And the live
    one takes a mutex immediately after fanning out, so it is largely serialised
    regardless.

    Left alone deliberately. Not "threading to fix later" — there is no threading
    here to fix. If integration ever measures too slow, the levers in order are:
    integrate at 5–10 Hz instead of per frame (depth is highly redundant
    frame-to-frame, and this is free), then voxel size, then marching cubes —
    which is the part that will actually hurt. Real parallelism belongs after all
    of those, and would need a persistent pool with atomic work claiming rather
    than a bigger `nthreads`: phone cores are asymmetric, so equal static blocks
    leave the big cores idling behind the little ones no matter how the count is
    chosen.
  * **`typedef size_t VertIndex`** in `mesh/Mesh.h` — 8 bytes per index, feeding
    a Vulkan index buffer that wants `VK_INDEX_TYPE_UINT32`. Costs a conversion
    pass on every mesh upload.
  * **`shared_ptr<Chunk>`** in the chunk hash map, so chunk lookups touch an
    atomic refcount.
  * **Eigen throughout**, where the rest of this project uses GLM. The surface is
    small enough to port (see the note in `CMakeLists.txt`), but there is no
    reason to until the extra dependency actually hurts.

## Updating from upstream

There is no meaningful upstream cadence to follow — the repository sees a commit
every year or two. If you ever do need to re-sync, the changes here are
mechanical enough to reapply by hand: grep for `LOCAL MODIFICATION:` to find all
of them.
