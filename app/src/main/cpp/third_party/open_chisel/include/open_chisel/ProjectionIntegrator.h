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

#ifndef PROJECTIONINTEGRATOR_H_
#define PROJECTIONINTEGRATOR_H_

#include <open_chisel/pointcloud/PointCloud.h>
#include <open_chisel/geometry/Geometry.h>
#include <open_chisel/geometry/Frustum.h>
#include <open_chisel/geometry/AABB.h>
#include <open_chisel/camera/PinholeCamera.h>
#include <open_chisel/camera/DepthImage.h>
#include <open_chisel/camera/ColorImage.h>
#include <open_chisel/Chunk.h>

#include <open_chisel/truncation/Truncator.h>
#include <open_chisel/weighting/Weighter.h>

namespace chisel
{

    class ProjectionIntegrator
    {
        public:
            ProjectionIntegrator();
            ProjectionIntegrator(const Truncator& t,
                                 const Weighter& w,
                                 float carvingDist,
                                 bool enableCarving,
                                 const Vec3List& centroids);

            ~ProjectionIntegrator();

            bool Integrate(const PointCloud& cloud, const Transform& cameraPose, Chunk* chunk, const std::vector<size_t>& idx) const;
            bool IntegratePointCloud(const PointCloud& cloud, const Transform& cameraPose, Chunk* chunk,  const std::vector<size_t>& idx) const;
            bool IntegrateColorPointCloud(const PointCloud& cloud, const Transform& cameraPose, Chunk* chunk,  const std::vector<size_t>& idx) const;

            template<class DataType> bool Integrate(const std::shared_ptr<const DepthImage<DataType> >& depthImage,
                                                    const PinholeCamera& camera,
                                                    const Transform& cameraPose, Chunk* chunk) const
            {
                assert(chunk != nullptr);

                Eigen::Vector3i numVoxels = chunk->GetNumVoxels();
                float resolution = chunk->GetVoxelResolutionMeters();
                Vec3 origin = chunk->GetOrigin();
                float diag = 2.0 * sqrt(3.0f) * resolution;
                Vec3 voxelCenter;
                bool updated = false;
                for (size_t i = 0; i < centroids.size(); i++)
                {
                    voxelCenter = centroids[i] + origin;
                    Vec3 voxelCenterInCamera = cameraPose.linear().transpose() * (voxelCenter - cameraPose.translation());
                    Vec3 cameraPos = camera.ProjectPoint(voxelCenterInCamera);

                    if (!camera.IsPointOnImage(cameraPos) || voxelCenterInCamera.z() < 0)
                        continue;

                    float voxelDist = voxelCenterInCamera.z();
                    float depth = depthImage->DepthAt((int)cameraPos(1), (int)cameraPos(0)); //depthImage->BilinearInterpolateDepth(cameraPos(0), cameraPos(1));

                    if(std::isnan(depth))
                    {
                        continue;
                    }

                    float truncation = truncator.GetTruncationDistance(depth);
                    float surfaceDist = depth - voxelDist;

                    if (fabs(surfaceDist) < truncation + diag)
                    {
                        // LOCAL MODIFICATION: per-pixel weight.
                        //
                        // Upstream passes a hardcoded 1.0f here -- every sample
                        // counts the same, and the `weighter` member is consulted
                        // only by IntegrateColor, which this app never calls. So
                        // the weighting policy was silently absent from the one
                        // path that runs.
                        //
                        // That matters because our depth samples are NOT of equal
                        // quality and we are told which is which. ARCore's raw
                        // depth is motion stereo, and its confidence map says how
                        // well the disparity search actually matched. A sample
                        // from a textured surface and a sample from a blank
                        // painted wall arrive in the same image and deserve very
                        // different votes.
                        //
                        // Weighting rather than DISCARDING is the point. A gate
                        // that drops everything below a threshold makes the
                        // reconstruction honest and full of holes -- a floor
                        // comes out as disconnected islands, because a floor is
                        // exactly the kind of low-texture surface the confidence
                        // map is pessimistic about. A weight lets the poor
                        // samples still build a continuous surface while being
                        // outvoted wherever a good one exists.
                        //
                        // Indexed at the SAME pixel the depth was read from, so
                        // it costs one array read and no extra projection.
                        float weight = 1.0f;
                        if (weights)
                        {
                            weight = weights->DepthAt((int)cameraPos(1), (int)cameraPos(0));
                        }

                        if (weight > 0.0f)
                        {
                            DistVoxel& voxel = chunk->GetDistVoxelMutable(i);
                            voxel.Integrate(surfaceDist, weight);
                            updated = true;
                        }
                    }
                    else if (enableVoxelCarving && surfaceDist > truncation + carvingDist)
                    {
                        DistVoxel& voxel = chunk->GetDistVoxelMutable(i);
                        if (voxel.GetWeight() > 0 && voxel.GetSDF() < 1e-5)
                        {
                            voxel.Carve();
                            updated = true;
                        }
                    }


                }
                return updated;
            }
            template<class DataType, class ColorType> bool IntegrateColor(const std::shared_ptr<const DepthImage<DataType> >& depthImage, const PinholeCamera& depthCamera, const Transform& depthCameraPose, const std::shared_ptr<const ColorImage<ColorType> >& colorImage, const PinholeCamera& colorCamera, const Transform& colorCameraPose, Chunk* chunk) const
            {
                    assert(chunk != nullptr);

                    float resolution = chunk->GetVoxelResolutionMeters();
                    Vec3 origin = chunk->GetOrigin();
                    float resolutionDiagonal = 2.0 * sqrt(3.0f) * resolution;
                    bool updated = false;
                    //std::vector<size_t> indexes;
                    //indexes.resize(centroids.size());
                    //for (size_t i = 0; i < centroids.size(); i++)
                    //{
                    //    indexes[i] = i;
                    //}

                    for (size_t i = 0; i < centroids.size(); i++)
                    //parallel_for(indexes.begin(), indexes.end(), [&](const size_t& i)
                    {
                        Color<ColorType> color;
                        Vec3 voxelCenter = centroids[i] + origin;
                        Vec3 voxelCenterInCamera = depthCameraPose.linear().transpose() * (voxelCenter - depthCameraPose.translation());
                        Vec3 cameraPos = depthCamera.ProjectPoint(voxelCenterInCamera);

                        if (!depthCamera.IsPointOnImage(cameraPos) || voxelCenterInCamera.z() < 0)
                        {
                            continue;
                        }

                        float voxelDist = voxelCenterInCamera.z();
                        float depth = depthImage->DepthAt((int)cameraPos(1), (int)cameraPos(0)); //depthImage->BilinearInterpolateDepth(cameraPos(0), cameraPos(1));

                        if(std::isnan(depth))
                        {
                            continue;
                        }

                        float truncation = truncator.GetTruncationDistance(depth);
                        float surfaceDist = depth - voxelDist;

                        if (std::abs(surfaceDist) < truncation + resolutionDiagonal)
                        {
                            Vec3 voxelCenterInColorCamera = colorCameraPose.linear().transpose() * (voxelCenter - colorCameraPose.translation());
                            Vec3 colorCameraPos = colorCamera.ProjectPoint(voxelCenterInColorCamera);
                            if(colorCamera.IsPointOnImage(colorCameraPos))
                            {
                                ColorVoxel& colorVoxel = chunk->GetColorVoxelMutable(i);

                                if (colorVoxel.GetWeight() < 5)
                                {
                                    int r = static_cast<int>(colorCameraPos(1));
                                    int c = static_cast<int>(colorCameraPos(0));
                                    colorImage->At(r, c, &color);
                                    colorVoxel.Integrate(color.red, color.green, color.blue, 1);
                                }
                            }

                            DistVoxel& voxel = chunk->GetDistVoxelMutable(i);
                            voxel.Integrate(surfaceDist, weighter.GetWeight(surfaceDist, truncation));

                            updated = true;
                        }
                        else if (enableVoxelCarving && surfaceDist > truncation + carvingDist)
                        {
                            DistVoxel& voxel = chunk->GetDistVoxelMutable(i);
                            if (voxel.GetWeight() > 0 && voxel.GetSDF() < 1e-5)
                            {
                                voxel.Carve();
                                updated = true;
                            }
                        }


                    }
                    //);

                    return updated;
            }

            inline const Truncator& GetTruncator() const { return truncator; }
            inline void SetTruncator(const Truncator& value) { truncator = value; }
            inline const Weighter& GetWeighter() const { return weighter; }
            inline void SetWeighter(const Weighter& value) { weighter = value; }

            inline float GetCarvingDist() const { return carvingDist; }
            inline bool IsCarvingEnabled() const { return enableVoxelCarving; }
            inline void SetCarvingDist(float dist) { carvingDist = dist; }
            inline void SetCarvingEnabled(bool enabled) { enableVoxelCarving = enabled; }

            inline void SetCentroids(const Vec3List& c) { centroids = c; }

            /**
             * LOCAL ADDITION: per-pixel integration weights, one float per depth
             * sample, same dimensions as the depth image. Null restores upstream
             * behaviour, which is a weight of 1.0 everywhere.
             *
             * A weight of 0 means "do not fuse this sample at all", which is how
             * a caller expresses a hard rejection without having to also blank
             * the depth map -- useful because the depth map's non-NaN extent is
             * what sizes the frustum.
             *
             * Set per frame, before IntegrateDepthScan. Held as a shared_ptr for
             * the same reason the depth image is: the integrator is const during
             * integration and must not own a dangling raw pointer if the caller
             * reallocates between frames.
             * */
            inline void SetWeights(const std::shared_ptr<const DepthImage<float> >& w) { weights = w; }
            inline void ClearWeights() { weights.reset(); }

        protected:
            Truncator truncator;
            Weighter weighter;
            float carvingDist;
            bool enableVoxelCarving;
            Vec3List centroids;
            std::shared_ptr<const DepthImage<float> > weights;
    };

} // namespace chisel 

#endif // PROJECTIONINTEGRATOR_H_ 
