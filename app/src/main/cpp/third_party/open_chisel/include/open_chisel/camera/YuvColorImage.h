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

#ifndef YUVCOLORIMAGE_H_
#define YUVCOLORIMAGE_H_

#include <algorithm>
#include <cstddef>
#include <cstdint>

namespace chisel
{

    // LOCAL ADDITION: the colour frame the integrator actually gets handed, and
    // the map from a depth pixel to a pixel of it.
    //
    // ## Why not ColorImage
    //
    // ColorImage assumes an interleaved BGR or BGRA buffer that it OWNS -- its
    // destructor is `delete [] data` -- and neither half is true here. A phone
    // camera delivers semi-planar YUV 4:2:0 (NV12 or NV21) in memory the capture
    // API owns and reclaims on the next frame, so the only correct thing to hold
    // is a borrowed pointer to each plane. Same rule as DepthImage's buffer:
    // consume it inside the frame, never store it.
    //
    // Converting a whole frame to RGB up front would make ColorImage usable, at
    // the cost of a 640x480 conversion and a ~900 KB buffer per frame, to serve
    // reads that only ever land on the voxels inside the truncation band. Sampling
    // on demand is fewer conversions and no allocation.
    //
    // ## The affine map, and why there is no second projection
    //
    // Upstream's IntegrateColor projects every voxel a SECOND time, through a
    // separate colour camera with its own extrinsic. That is the right shape for
    // a rig where colour and depth are different physical sensors. On a phone
    // they are the same sensor: the depth map is a centre crop of the camera
    // image followed by a uniform resample, so the two share a pose exactly and
    // differ only in intrinsics.
    //
    // Two pinhole cameras at the same pose relate by an affine map on pixels,
    // because the (X/Z, Y/Z) they both project cancels:
    //
    //     u_colour = (fx_c / fx_d) * u_depth + (cx_c - (fx_c / fx_d) * cx_d)
    //
    // So the pixel this class is sampled at costs one multiply and one add per
    // axis, applied to the projection the depth path has ALREADY done. No second
    // transform, no second frustum test: a pixel inside the depth image is inside
    // the crop it was made from, hence inside this image.
    //
    // scale is the same on both axes whenever the crop is centred, which is what
    // makes it a useful self-check -- see the probe in Reconstruction.cpp.
    struct YuvColorImage
    {
        /** Full-resolution luma. Borrowed. */
        const uint8_t* luma = nullptr;
        int lumaRowStride = 0;

        /** Half width, half height, two interleaved bytes per texel. Borrowed. */
        const uint8_t* chroma = nullptr;
        int chromaRowStride = 0;

        /** True when the first byte of each chroma pair is V (NV21), not U (NV12). */
        bool vFirst = false;

        int width = 0;
        int height = 0;

        /** Depth pixel to colour pixel, per axis. See the class comment. */
        float scaleX = 1.0f;
        float offsetX = 0.0f;
        float scaleY = 1.0f;
        float offsetY = 0.0f;

        inline bool IsValid() const
        {
            return luma != nullptr && chroma != nullptr && width > 0 && height > 0 &&
                   lumaRowStride > 0 && chromaRowStride > 0;
        }

        /**
         * BT.601, studio swing, into 8-bit sRGB.
         *
         * This is the same arithmetic composite.frag runs on the camera feed, and
         * keeping the two identical is the point rather than an accident: the
         * reconstruction is drawn directly in front of the feed it was sampled
         * from, so any disagreement between these coefficients and that shader's
         * shows up as reconstructed geometry that is visibly a different colour
         * from the pixels around it.
         *
         * Studio swing means luma occupies 16..235 and chroma 16..240. Treating
         * it as full range is the classic mistake and reads as milky blacks.
         *
         * What comes out is gamma encoded, which is what an 8-bit colour channel
         * should hold -- linear 8-bit bands visibly in the darks, and indoors is
         * where a camera feed spends much of its range. The shader linearises on
         * the way out.
         */
        inline void SampleAtDepthPixel(float depthColumn, float depthRow, uint8_t* rgb) const
        {
            const int column = std::clamp(static_cast<int>(scaleX * depthColumn + offsetX), 0,
                                          width - 1);
            const int row = std::clamp(static_cast<int>(scaleY * depthRow + offsetY), 0,
                                       height - 1);

            const float y =
                    (static_cast<float>(luma[static_cast<size_t>(row) * lumaRowStride + column]) /
                             255.0f -
                     0.0625f) *
                    1.164383f;

            // Chroma is half resolution on both axes and interleaved two bytes
            // apart, so the pair for this pixel starts at (row/2, (column/2)*2).
            const uint8_t* pair =
                    chroma + static_cast<size_t>(row / 2) * chromaRowStride + (column / 2) * 2;
            const float first = static_cast<float>(pair[0]) / 255.0f - 0.5f;
            const float second = static_cast<float>(pair[1]) / 255.0f - 0.5f;
            const float u = vFirst ? second : first;
            const float v = vFirst ? first : second;

            rgb[0] = ToByte(y + 1.596027f * v);
            rgb[1] = ToByte(y - 0.391762f * u - 0.812968f * v);
            rgb[2] = ToByte(y + 2.017232f * u);
        }

    private:
        static inline uint8_t ToByte(float channel)
        {
            return static_cast<uint8_t>(std::clamp(channel, 0.0f, 1.0f) * 255.0f + 0.5f);
        }
    };

} // namespace chisel

#endif // YUVCOLORIMAGE_H_
