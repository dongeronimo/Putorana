// The MIT License (MIT)
// Copyright (c) 2014 Matthew Klingensmith and Ivan Dryanovski
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all
// copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
// SOFTWARE.

#ifndef DISTVOXEL_H_
#define DISTVOXEL_H_

#include <limits>
#include <stdint.h>

#include <open_chisel/FixedPointFloat.h>

namespace chisel
{

    class DistVoxel
    {
        public:
            DistVoxel();

            // LOCAL MODIFICATION: no destructor, declared or defined.
            //
            // Upstream declares `virtual ~DistVoxel()` with an empty body.
            // Nothing in the library derives from this class, so the vtable
            // pointer bought nothing and cost 8 bytes on top of 8 bytes of
            // payload -- half of every cache line in the integration loop was
            // vptr. Removing the declaration entirely (rather than just the
            // `virtual`) additionally makes the type trivially destructible,
            // so tearing down a std::vector<DistVoxel> becomes a no-op instead
            // of a loop over millions of empty calls.

            inline float GetSDF() const
            {
                return sdf;
            }

            inline void SetSDF(const float& distance)
            {
                sdf = distance;
            }

            inline float GetWeight() const { return weight; }
            inline void SetWeight(const float& w) { weight = w; }

            // LOCAL MODIFICATION: optional weight ceiling.
            //
            // maxWeight <= 0 keeps upstream behaviour exactly, which is an
            // unbounded running mean. That is correct for a static scene and
            // wrong for one that changes: a voxel observed three hundred times
            // has weight 300, so a new observation moves it by 1/301 and the
            // reconstruction cannot be corrected by fresh evidence at all.
            //
            // Only the STORED weight is clamped. The distance still uses the
            // true running mean for this update, which is what keeps the value
            // a distance in metres rather than a decayed approximation of one.
            inline void Integrate(const float& distUpdate, const float& weightUpdate,
                                  const float& maxWeight = 0.0f)
            {
                float oldSDF = GetSDF();
                float oldWeight = GetWeight();
                float newDist = (oldWeight * oldSDF + weightUpdate * distUpdate) / (weightUpdate + oldWeight);
                float newWeight = oldWeight + weightUpdate;
                if (maxWeight > 0.0f && newWeight > maxWeight)
                {
                    newWeight = maxWeight;
                }
                SetSDF(newDist);
                SetWeight(newWeight);

            }

            // LOCAL MODIFICATION: carving decays the belief instead of voting
            // against it.
            //
            // Upstream is `Integrate(0.0, 1.5)`, and it cannot remove a surface.
            // Two independent reasons, either of which alone is fatal:
            //
            //   1. Integrate ADDS to the weight. A voxel at weight 200 moves by
            //      1.5/201.5, under one percent, and comes out heavier than it
            //      went in. Every carve makes the next one weaker.
            //   2. It pulls the SDF toward ZERO, which is the surface, not free
            //      space. The eligibility test upstream only admits voxels whose
            //      SDF is already at or below zero, so carving walks them
            //      asymptotically up to 0 from below and the sign never flips.
            //      Marching cubes keys on sign changes, so the triangle stays.
            //
            // Observing free space is evidence AGAINST what this voxel holds,
            // not a competing measurement of it. So the accumulated weight is
            // decayed, and once it falls below the floor the voxel is Reset to
            // unobserved: sdf 99999, weight 0. ChunkManager treats weight below
            // 1e-15 as unobserved and skips the whole cube, so that is what
            // actually deletes the geometry.
            //
            // decay is per observation. At 30 fps, 0.85 removes a voxel carried
            // at weight 200 in about a second of looking at where it used to be.
            inline void Carve(const float& decay, const float& minWeight)
            {
                const float decayed = GetWeight() * decay;
                if (decayed <= minWeight)
                {
                    Reset();
                    return;
                }
                SetWeight(decayed);
            }

            inline void Reset()
            {
                sdf = 99999;
                weight = 0;
            }

        protected:
           float sdf;
           float weight;
    };

} // namespace chisel 

#endif // DISTVOXEL_H_ 
