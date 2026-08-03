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

#ifndef WEIGHTER_H_
#define WEIGHTER_H_

namespace chisel
{

    // LOCAL MODIFICATION: this was an abstract base class with a pure virtual
    // GetWeight(), and ConstantWeighter was its only subclass in the entire
    // codebase -- one implementation behind a vtable. Like Truncator, it was
    // called from inside the per-voxel loop, so the dispatch blocked inlining
    // of a single divide. Merged into one concrete class, held by value.
    //
    //     w(surfaceDist, tau) = weight / (2 * tau)
    //
    // Note what this formula does and does not say. The weight does NOT depend
    // on distance to the camera directly. It depends on tau, and tau grows
    // quadratically with the depth reading -- so a distant sample gets a wide
    // truncation band and, through this division, a proportionally smaller
    // weight. The two are coupled: one knob, not two.
    //
    // surfaceDist is accepted and ignored, exactly as upstream did. It is the
    // hook for a signed weighting profile -- trusting the near side of the band
    // (space the camera actually saw through) more than the far side (a guess
    // about what lies behind the surface). Left unused so that turning it on
    // later is a change to this function and nothing else.
    class Weighter
    {
        public:
            Weighter() = default;

            explicit Weighter(float w) : weight(w)
            {

            }

            inline float GetWeight(float /*surfaceDist*/, float truncationDist) const
            {
                return weight / (2.0f * truncationDist);
            }

            inline float GetBaseWeight() const { return weight; }
            inline void SetBaseWeight(float value) { weight = value; }

        protected:
            float weight = 1.0f;
    };

} // namespace chisel 

#endif // WEIGHTER_H_ 
