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

#ifndef TRUNCATOR_H_
#define TRUNCATOR_H_

#include <cmath>

namespace chisel
{

    // LOCAL MODIFICATION: this was an abstract base class with a pure virtual
    // GetTruncationDistance(), subclassed by ConstantTruncator and
    // QuadraticTruncator, and held by ProjectionIntegrator as a shared_ptr.
    //
    // Why that mattered: GetTruncationDistance is called INSIDE the per-voxel
    // loop of ProjectionIntegrator::Integrate. A 16^3 chunk is 4096 voxels, and
    // every chunk in the view frustum is swept every frame, so the virtual call
    // ran millions of times per second on a function whose body is three
    // multiplies. The dispatch itself was the smaller cost; the real loss was
    // that the compiler could not inline the body into the loop, could not
    // hoist the coefficient loads out of it, and could not vectorise across
    // voxels.
    //
    // The fix is to notice that the quadratic form SUBSUMES the constant one --
    // a constant truncation is just a = b = 0 -- so a single concrete class
    // covers both cases with no loss of configurability. The truncation model
    // stays a runtime parameter; only the dispatch disappears.
    //
    //     tau(d) = |a*d^2 + b*d + c| * s
    //
    // Held BY VALUE in ProjectionIntegrator now, which also removes a pointer
    // chase and a shared_ptr from the hot path.
    class Truncator
    {
        public:
            Truncator() = default;

            Truncator(float quadratic, float linear, float constant, float scale) :
                quadraticTerm(quadratic), linearTerm(linear),
                constantTerm(constant), scalingFactor(scale)
            {

            }

            // The degenerate case: tau(d) = |c| for every reading. This replaces
            // the old ConstantTruncator.
            static Truncator Constant(float truncationDistance)
            {
                return Truncator(0.0f, 0.0f, truncationDistance, 1.0f);
            }

            // Upstream wrote this as
            //     std::abs(a * pow(reading, 2) + b * reading + c) * s
            // Horner's form below is the same polynomial with one fewer multiply
            // and, more importantly, no call to pow() -- which through the old
            // virtual boundary was an actual libm call, per voxel, to compute a
            // square.
            inline float GetTruncationDistance(float reading) const
            {
                return std::fabs((quadraticTerm * reading + linearTerm) * reading + constantTerm)
                       * scalingFactor;
            }

            inline float GetQuadraticTerm() const { return quadraticTerm; }
            inline float GetLinearTerm() const { return linearTerm; }
            inline float GetConstantTerm() const { return constantTerm; }
            inline float GetScalingFactor() const { return scalingFactor; }
            inline void SetQuadraticTerm(float value) { quadraticTerm = value; }
            inline void SetLinearTerm(float value) { linearTerm = value; }
            inline void SetConstantTerm(float value) { constantTerm = value; }
            inline void SetScalingFactor(float value) { scalingFactor = value; }

        protected:
            float quadraticTerm = 0.0f;
            float linearTerm = 0.0f;
            float constantTerm = 0.0f;
            float scalingFactor = 1.0f;
    };

} // namespace chisel

#endif // TRUNCATOR_H_
