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
                // LOCAL MODIFICATION: half a voxel diagonal, not two of them.
                //
                // Upstream is `2.0 * sqrt(3) * resolution`, which at 4 cm voxels
                // is 0.1386 m of padding on a configured truncation of 0.06. The
                // band that actually gets integrated came out at +-0.1986, more
                // than three times the truncation that was asked for.
                //
                // The padding exists because the test below is on the voxel
                // CENTRE while the voxel has extent. The furthest any part of a
                // voxel sits from its centre is half a diagonal,
                // sqrt(3)/2 * resolution = 0.0346 m here, so that is the value
                // the geometry justifies. Upstream used four times it.
                //
                // This is also what made carving unreachable. The carve branch
                // below is an `else if`, so it cannot fire until surfaceDist
                // exceeds truncation + diag, and carvingDist (0.05) was smaller
                // than diag (0.1386). The configured carving distance therefore
                // did nothing at all, and raising it could only ever push the
                // start of carving further away. With the padding at 0.0346 the
                // carve threshold becomes truncation + carvingDist for real, and
                // an object removed from a table stops being permanently inside
                // the band of the table behind it.
                //
                // Second effect, and it is a bonus rather than the goal: fewer
                // voxels pass the test, so integration touches less per frame.
                //
                // Half a diagonal is the value the geometry justifies and it was
                // MEASURED TOO SMALL on the device: the surface came back shot
                // through with holes, because marching cubes needs all eight
                // corners of a cube observed and a band that thin leaves too
                // many of them outside it. One full diagonal is the setting that
                // holds, and it is still half of upstream's.
                float diag = 1.0 * sqrt(3.0f) * resolution;
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

                            // Counted, not acted on, and the count is the point.
                            //
                            // This voxel is in FRONT of the measured surface but
                            // still inside the truncation band, so it lands here
                            // rather than in the carving branch below and is
                            // pushed toward positive slowly instead of being
                            // erased. The two branches do not partition the way
                            // the configuration suggests: carvingDist is 0.05
                            // and truncation is 0.06, but this branch tests
                            // against truncation + diag, and diag is 0.1386 at
                            // 4 cm voxels. Everything nearer than 0.1986 m in
                            // front of a surface is integrated, never carved.
                            //
                            // Which means an object THINNER than that cannot be
                            // carved away when it is removed, because whatever
                            // was behind it is still inside the band. Telling
                            // that apart from "carving is not firing" is what
                            // this counter is for.
                            if (surfaceDist > 0.0f && voxel.GetWeight() > 0.0f)
                            {
                                ++nearFrontVoxels;
                            }

                            voxel.Integrate(surfaceDist, weight, maxWeight);
                            updated = true;
                        }
                    }
                    else if (enableVoxelCarving && surfaceDist > truncation + diag + carvingDist)
                    {
                        // This voxel sits well in FRONT of the measured surface:
                        // the ray passed through it and hit something further
                        // away, so it was observed to be empty.
                        //
                        // LOCAL MODIFICATION: `+ diag`, which turns carvingDist
                        // into a DEAD ZONE above the integration band rather
                        // than a threshold competing with it.
                        //
                        // Upstream tests against truncation + carvingDist while
                        // the branch above tests against truncation + diag, and
                        // this is an else-if. Two consequences, both bad. When
                        // carvingDist is the smaller of the two, which it was,
                        // the whole parameter is inert and carving silently
                        // starts wherever the integration band happens to end.
                        // And the two branches then share an exact boundary, so
                        // a voxel sitting on it flips between being integrated
                        // and being carved from frame to frame as depth noise
                        // crosses the line. That was visible on the device as
                        // geometry flickering with no object moving.
                        //
                        // Written this way there is a band of carvingDist metres
                        // where a voxel is neither fused nor eroded, and nothing
                        // can oscillate across it in one frame of noise. It also
                        // makes the parameter monotone in the direction its name
                        // implies: larger means carving starts further from the
                        // surface, which is slower removal and steadier
                        // geometry. That trade is the reason it exists.
                        //
                        // LOCAL MODIFICATION: upstream also required
                        // `voxel.GetSDF() < 1e-5` here, which admits only voxels
                        // already at or behind a surface. That is the wrong half
                        // of the population. A voxel sitting just in front of an
                        // object that has been taken away carries a small
                        // POSITIVE SDF and never qualified, so the near face of
                        // a removed object was structurally uncarvable.
                        //
                        // The weight test stays, because a voxel that was never
                        // observed has nothing to carve and Reset would be a
                        // write for no reason.
                        DistVoxel& voxel = chunk->GetDistVoxelMutable(i);
                        if (voxel.GetWeight() > 0.0f)
                        {
                            voxel.Carve(carvingDecay, carvingMinWeight);
                            ++carvedVoxels;
                            if (voxel.GetWeight() <= 0.0f)
                            {
                                ++clearedVoxels;
                            }
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
                                voxel.Carve(carvingDecay, carvingMinWeight);
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

            /**
             * LOCAL ADDITION: ceiling on accumulated per-voxel weight. 0 keeps
             * upstream's unbounded running mean. See DistVoxel::Integrate.
             * */
            inline float GetMaxWeight() const { return maxWeight; }
            inline void SetMaxWeight(float value) { maxWeight = value; }

            /**
             * LOCAL ADDITION: what one observation of free space does to a
             * voxel's accumulated weight, and the floor at which it is Reset to
             * unobserved. See DistVoxel::Carve.
             * */
            inline void SetCarvingDecay(float decay, float minWeight)
            {
                carvingDecay = decay;
                carvingMinWeight = minWeight;
            }

            /**
             * LOCAL ADDITION: where the voxels in front of a surface went, since
             * the last ResetVoxelCounters.
             *
             *   carved       decayed by the carving branch
             *   cleared      of those, the ones that reached weight 0 and are
             *                now unobserved, which is what deletes geometry
             *   nearFront    in front of the surface but inside the truncation
             *                band, so integrated rather than carved
             *
             * Plain counters rather than atomics, and that is safe only because
             * integration is single-threaded here: parallel_for is inert at the
             * chunk counts this runs at, and the one call site that fans out is
             * IntegrateDepthScanColor, which this project does not use. If that
             * ever changes these have to change with it.
             * */
            inline uint32_t GetCarvedVoxels() const { return carvedVoxels; }
            inline uint32_t GetClearedVoxels() const { return clearedVoxels; }
            inline uint32_t GetNearFrontVoxels() const { return nearFrontVoxels; }
            inline void ResetVoxelCounters() const
            {
                carvedVoxels = 0;
                clearedVoxels = 0;
                nearFrontVoxels = 0;
            }

        protected:
            Truncator truncator;
            Weighter weighter;
            float carvingDist;
            bool enableVoxelCarving;
            Vec3List centroids;
            std::shared_ptr<const DepthImage<float> > weights;
            float maxWeight = 0.0f;
            float carvingDecay = 0.85f;
            float carvingMinWeight = 0.5f;

            // Mutable so Integrate can stay const. See the accessors above for
            // why unsynchronised counters are safe in this configuration.
            mutable uint32_t carvedVoxels = 0;
            mutable uint32_t clearedVoxels = 0;
            mutable uint32_t nearFrontVoxels = 0;
    };

} // namespace chisel 

#endif // PROJECTIONINTEGRATOR_H_ 
