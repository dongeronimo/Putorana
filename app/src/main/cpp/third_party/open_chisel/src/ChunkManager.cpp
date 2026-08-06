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

#include <assert.h>
#include <open_chisel/threading/Threading.h>
#include <open_chisel/ChunkManager.h>
#include <open_chisel/geometry/Frustum.h>
#include <open_chisel/geometry/AABB.h>
#include <open_chisel/marching_cubes/MarchingCubes.h>
#include <open_chisel/geometry/Raycast.h>
#include <open_chisel/ProjectionIntegrator.h>
#include <open_chisel/truncation/Truncator.h>
#include <iostream>

namespace chisel
{

    ChunkManager::ChunkManager() :
            chunkSize(16, 16, 16), voxelResolutionMeters(0.03)
    {
        CacheCentroids();
    }

    ChunkManager::~ChunkManager()
    {

    }

    ChunkManager::ChunkManager(const Eigen::Vector3i& size, float res, bool color) :
            chunkSize(size), voxelResolutionMeters(res), useColor(color)
    {
        CacheCentroids();
    }

    void ChunkManager::CacheCentroids()
    {
        halfVoxel = Vec3(voxelResolutionMeters, voxelResolutionMeters, voxelResolutionMeters) * 0.5f;
        centroids.resize(static_cast<size_t>(chunkSize(0) * chunkSize(1) * chunkSize(2)));
        int i = 0;
        for (int z = 0; z < chunkSize(2); z++)
        {
            for(int y = 0; y < chunkSize(1); y++)
            {
                for(int x = 0; x < chunkSize(0); x++)
                {
                    centroids[i] = Vec3(x, y, z) * voxelResolutionMeters + halfVoxel;
                    i++;
                }
            }
        }

        cubeIndexOffsets << 0, 1, 1, 0, 0, 1, 1, 0,
                            0, 0, 1, 1, 0, 0, 1, 1,
                            0, 0, 0, 0, 1, 1, 1, 1;
    }

    void ChunkManager::GetChunkIDsIntersecting(const AABB& box, ChunkIDList* chunkList)
    {
        assert(chunkList != nullptr);

        ChunkID minID = GetIDAt(box.min);
        ChunkID maxID = GetIDAt(box.max) + Eigen::Vector3i(1, 1, 1);

        for (int x = minID(0); x < maxID(0); x++)
        {
            for (int y = minID(1); y < maxID(1); y++)
            {
                for (int z = minID(2); z < maxID(2); z++)
                {
                    chunkList->push_back(ChunkID(x, y, z));
                }
            }
        }
    }


    void ChunkManager::RecomputeMesh(const ChunkID& chunkID, std::mutex& mutex)
    {
        mutex.lock();
        if (!HasChunk(chunkID))
        {
            mutex.unlock();
            return;
        }

        MeshPtr mesh;
        if (!HasMesh(chunkID))
        {
            mesh = std::allocate_shared<Mesh>(Eigen::aligned_allocator<Mesh>());
        }
        else
        {
            mesh = GetMesh(chunkID);
        }


        ChunkPtr chunk = GetChunk(chunkID);
        mutex.unlock();

        GenerateMesh(chunk, mesh.get());

        if(useColor)
        {
            ColorizeMesh(mesh.get());
        }

        ComputeNormalsFromGradients(mesh.get());

        mutex.lock();
        if(!mesh->vertices.empty())
            allMeshes[chunkID] = mesh;
        mutex.unlock();
    }

    void ChunkManager::RecomputeMeshes(const ChunkSet& chunkMeshes)
    {

        if (chunkMeshes.empty())
        {
            return;
        }

        std::mutex mutex;
        for (const std::pair<ChunkID, bool>& chunk : chunkMeshes)
        //parallel_for(chunks.begin(), chunks.end(), [this, &mutex](const ChunkID& chunkID)
        {
            if (chunk.second)
              this->RecomputeMesh(ChunkID(chunk.first), mutex);
        }

    }

    void ChunkManager::CreateChunk(const ChunkID& id)
    {
        AddChunk(std::allocate_shared<Chunk>(Eigen::aligned_allocator<Chunk>(), id, chunkSize, voxelResolutionMeters, useColor));
    }

    void ChunkManager::Reset()
    {
        allMeshes.clear();
        chunks.clear();
    }

    void ChunkManager::GetChunkIDsIntersecting(const Frustum& frustum, ChunkIDList* chunkList)
    {
        assert(chunkList != nullptr);

        AABB frustumAABB;
        frustum.ComputeBoundingBox(&frustumAABB);

        ChunkID minID = GetIDAt(frustumAABB.min);
        ChunkID maxID = GetIDAt(frustumAABB.max) + Eigen::Vector3i(1, 1, 1);

        //printf("FrustumAABB: %f %f %f %f %f %f\n", frustumAABB.min.x(), frustumAABB.min.y(), frustumAABB.min.z(), frustumAABB.max.x(), frustumAABB.max.y(), frustumAABB.max.z());
        //printf("Frustum min: %d %d %d max: %d %d %d\n", minID.x(), minID.y(), minID.z(), maxID.x(), maxID.y(), maxID.z());
        for (int x = minID(0) - 1; x <= maxID(0) + 1; x++)
        {
            for (int y = minID(1) - 1; y <= maxID(1) + 1; y++)
            {
                for (int z = minID(2) - 1; z <= maxID(2) + 1; z++)
                {
                    Vec3 min = Vec3(x * chunkSize(0), y * chunkSize(1), z * chunkSize(2)) * voxelResolutionMeters;
                    Vec3 max = min + chunkSize.cast<float>() * voxelResolutionMeters;
                    AABB chunkBox(min, max);
                    if(frustum.Intersects(chunkBox))
                    {
                        chunkList->push_back(ChunkID(x, y, z));
                    }
                }
            }
        }

        //printf("%lu chunks intersect frustum\n", chunkList->size());
    }

    void ChunkManager::GetChunkIDsIntersecting
    (
            const PointCloud& cloud,
            const Transform& cameraTransform,
            const ProjectionIntegrator& integrator,
            float maxDist,
            ChunkPointMap* chunkList
    )
    {
        assert(!!chunkList);
        chunkList->clear();
        const float roundX = 1.0f / (chunkSize.x() * voxelResolutionMeters);
        const float roundY = 1.0f / (chunkSize.y() * voxelResolutionMeters);
        const float roundZ = 1.0f / (chunkSize.z() * voxelResolutionMeters);
        const Truncator& truncator = integrator.GetTruncator();

        Point3 minVal(-std::numeric_limits<int>::max(), -std::numeric_limits<int>::max(), -std::numeric_limits<int>::max());
        Point3 maxVal(std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), std::numeric_limits<int>::max());
        size_t numPoints = cloud.GetPoints().size();
        Vec3 start = cameraTransform.translation();
        for (size_t i = 0; i < numPoints; i++)
        {
            const Vec3& point = cloud.GetPoints().at(i);
            Vec3 end = cameraTransform * point;
            float truncation = truncator.GetTruncationDistance(point.z());
            Vec3 dir = (end - start).normalized();
            Vec3 truncEnd = end + dir * truncation;
            Vec3 truncStart = end - dir * truncation;
            Vec3 startInt = Vec3(truncStart.x() * roundX , truncStart.y() * roundY, truncStart.z() * roundZ);
            Vec3 endInt = Vec3(truncEnd.x() * roundX, truncEnd.y() * roundY, truncEnd.z() * roundZ);

            Point3List intersectingChunks;
            Raycast(startInt, endInt, minVal, maxVal, &intersectingChunks);

            for (const Point3& id : intersectingChunks)
            {
                if(chunkList->find(id) == chunkList->end())
                    (*chunkList)[id] = std::vector<size_t>();

                (*chunkList)[id].push_back(i);
            }
        }

    }

    void ChunkManager::ExtractInsideVoxelMesh(const ChunkPtr& chunk, const Eigen::Vector3i& index, const Vec3& coords, std::unordered_map<uint64_t, VertIndex>* edgeVertices, VertIndex* nextMeshIndex, Mesh* mesh)
    {
        assert(mesh != nullptr);
        Eigen::Matrix<float, 3, 8> cubeCoordOffsets = cubeIndexOffsets.cast<float>() * voxelResolutionMeters;
        Eigen::Matrix<float, 3, 8> cornerCoords;
        Eigen::Matrix<float, 8, 1> cornerSDF;
        bool allNeighborsObserved = true;
        for (int i = 0; i < 8; ++i)
        {
            Eigen::Vector3i corner_index = index + cubeIndexOffsets.col(i);
            const DistVoxel& thisVoxel = chunk->GetDistVoxel(corner_index.x(), corner_index.y(), corner_index.z());

            // Do not extract a mesh here if one of the corner is unobserved and
            // outside the truncation region.
            if (thisVoxel.GetWeight() <= 1e-15)
            {
                allNeighborsObserved = false;
                break;
            }
            cornerCoords.col(i) = coords + cubeCoordOffsets.col(i);
            cornerSDF(i) = thisVoxel.GetSDF();
        }

        if (allNeighborsObserved)
        {
            MarchingCubes::MeshCube(cornerCoords, cornerSDF, index, edgeVertices, nextMeshIndex, mesh);
        }
    }

    void ChunkManager::ExtractBorderVoxelMesh(const ChunkPtr& chunk, const Eigen::Vector3i& index, const Eigen::Vector3f& coordinates, std::unordered_map<uint64_t, VertIndex>* edgeVertices, VertIndex* nextMeshIndex, Mesh* mesh)
    {
        const Eigen::Matrix<float, 3, 8> cubeCoordOffsets = cubeIndexOffsets.cast<float>() * voxelResolutionMeters;
        Eigen::Matrix<float, 3, 8> cornerCoords;
        Eigen::Matrix<float, 8, 1> cornerSDF;
        bool allNeighborsObserved = true;
        for (int i = 0; i < 8; ++i)
        {
            Eigen::Vector3i cornerIDX = index + cubeIndexOffsets.col(i);

            if (chunk->IsCoordValid(cornerIDX.x(), cornerIDX.y(), cornerIDX.z()))
            {
                const DistVoxel& thisVoxel = chunk->GetDistVoxel(cornerIDX.x(), cornerIDX.y(), cornerIDX.z());
                // Do not extract a mesh here if one of the corners is unobserved
                // and outside the truncation region.
                if (thisVoxel.GetWeight() <= 1e-15)
                {
                    allNeighborsObserved = false;
                    break;
                }
                cornerCoords.col(i) = coordinates + cubeCoordOffsets.col(i);
                cornerSDF(i) = thisVoxel.GetSDF();
            }
            else
            {
                Eigen::Vector3i chunkOffset = Eigen::Vector3i::Zero();


                for (int j = 0; j < 3; j++)
                {
                    if (cornerIDX(j) < 0)
                    {
                        chunkOffset(j) = -1;
                        cornerIDX(j) = chunkSize(j) - 1;
                    }
                    else if(cornerIDX(j) >= chunkSize(j))
                    {
                        chunkOffset(j) = 1;
                        cornerIDX(j) = 0;
                    }
                }

                ChunkID neighborID = chunkOffset + chunk->GetID();

                if (HasChunk(neighborID))
                {
                    const ChunkPtr& neighborChunk = GetChunk(neighborID);
                    if(!neighborChunk->IsCoordValid(cornerIDX.x(), cornerIDX.y(), cornerIDX.z()))
                    {
                        allNeighborsObserved = false;
                        break;
                    }

                    const DistVoxel& thisVoxel = neighborChunk->GetDistVoxel(cornerIDX.x(), cornerIDX.y(), cornerIDX.z());
                    // Do not extract a mesh here if one of the corners is unobserved
                    // and outside the truncation region.
                    if (thisVoxel.GetWeight() <= 1e-15)
                    {
                        allNeighborsObserved = false;
                        break;
                    }
                    cornerCoords.col(i) = coordinates + cubeCoordOffsets.col(i);
                    cornerSDF(i) = thisVoxel.GetSDF();
                }
                else
                {
                    allNeighborsObserved = false;
                    break;
                }

            }

        }

        if (allNeighborsObserved)
        {
            MarchingCubes::MeshCube(cornerCoords, cornerSDF, index, edgeVertices, nextMeshIndex, mesh);
        }
    }


    void ChunkManager::GenerateMesh(const ChunkPtr& chunk, Mesh* mesh)
    {
        assert(mesh != nullptr);

        mesh->Clear();
        const int maxX = chunkSize(0);
        const int maxY = chunkSize(1);
        const int maxZ = chunkSize(2);


        Eigen::Vector3i index;
        VoxelID i = 0;
        VertIndex nextIndex = 0;

        // LOCAL MODIFICATION: grid edge -> the vertex already emitted for it.
        //
        // A marching-cubes vertex lives on an edge of the voxel grid, and that
        // edge is shared by the four cubes around it, so without this every
        // vertex is emitted up to four times over -- and three times over within
        // a single cube whose triangles meet. See MarchingCubes::MeshCube.
        //
        // Local rather than a member: RecomputeMesh takes a mutex per chunk and
        // is meant to be callable from more than one thread, and a shared map
        // would quietly make that false.
        std::unordered_map<uint64_t, VertIndex> edgeVertices;
        // A chunk that is full of surface emits on the order of a thousand
        // vertices. Reserving costs one allocation and saves the rehashing.
        edgeVertices.reserve(1024);

        // For voxels not bordering the outside, we can use a more efficient function.
        for (index.z() = 0; index.z() < maxZ - 1; index.z()++)
        {
            for (index.y() = 0; index.y() < maxY - 1; index.y()++)
            {
                for (index.x() = 0; index.x() < maxX - 1; index.x()++)
                {
                    i = chunk->GetVoxelID(index.x(), index.y(), index.z());
                    ExtractInsideVoxelMesh(chunk, index, centroids.at(i) + chunk->GetOrigin(), &edgeVertices, &nextIndex, mesh);
                }
            }
        }

        // Max X plane (takes care of max-Y corner as well).
        i = 0;
        index.x() = maxX - 1;
        for (index.z() = 0; index.z() < maxZ - 1; index.z()++)
        {
            for (index.y() = 0; index.y() < maxY; index.y()++)
            {
                i = chunk->GetVoxelID(index.x(), index.y(), index.z());
                ExtractBorderVoxelMesh(chunk, index, centroids.at(i) + chunk->GetOrigin(), &edgeVertices, &nextIndex, mesh);
            }
        }

        // Max Y plane.
        i = 0;
        index.y() = maxY - 1;
        for (index.z() = 0; index.z() < maxZ - 1; index.z()++)
        {
            for (index.x() = 0; index.x() < maxX - 1; index.x()++)
            {
                i = chunk->GetVoxelID(index.x(), index.y(), index.z());
                ExtractBorderVoxelMesh(chunk, index, centroids.at(i) + chunk->GetOrigin(), &edgeVertices, &nextIndex, mesh);
            }
        }

        // Max Z plane (also takes care of corners).
        i = 0;
        index.z() = maxZ - 1;
        for (index.y() = 0; index.y() < maxY; index.y()++)
        {
            for (index.x() = 0; index.x() < maxX; index.x()++)
            {
                i = chunk->GetVoxelID(index.x(), index.y(), index.z());
                ExtractBorderVoxelMesh(chunk, index, centroids.at(i) + chunk->GetOrigin(), &edgeVertices, &nextIndex, mesh);
            }
        }

        // LOCAL MODIFICATION: normalise the accumulated face normals.
        //
        // MeshCube adds each triangle's unnormalised cross product to all three
        // of its vertices, so a shared vertex ends up with an area-weighted sum
        // over the triangles that meet there. That is only the FALLBACK --
        // ComputeNormalsFromGradients overwrites it with the SDF gradient
        // wherever the lookup succeeds -- but it has to be a unit vector when
        // the lookup fails, or the shader normalises a zero-length vector and
        // gets NaN, which reads as a black hole in the surface.
        for (Vec3& normal : mesh->normals)
        {
            const float magnitude = normal.norm();
            if (magnitude > 1e-12f)
            {
                normal /= magnitude;
            }
        }

        // The old assertion here was vertices.size() == indices.size(), which
        // was true only because upstream emitted a triangle soup. Sharing
        // vertices is the whole point now, so the invariant is the weaker and
        // more meaningful one: whole triangles, and no index out of range.
        assert(mesh->vertices.size() == mesh->normals.size());
        assert(mesh->indices.size() % 3 == 0);
        assert(mesh->vertices.size() <= mesh->indices.size());
    }

    bool ChunkManager::GetSDFAndGradient(const Eigen::Vector3f& pos, double* dist, Eigen::Vector3f* grad)
    {
        Eigen::Vector3f posf = Eigen::Vector3f(std::floor(pos.x() / voxelResolutionMeters) * voxelResolutionMeters + voxelResolutionMeters / 2.0f,
                std::floor(pos.y() / voxelResolutionMeters) * voxelResolutionMeters + voxelResolutionMeters / 2.0f,
                std::floor(pos.z() / voxelResolutionMeters) * voxelResolutionMeters + voxelResolutionMeters / 2.0f);
        if (!GetSDF(posf, dist)) return false;
        double ddxplus, ddyplus, ddzplus = 0.0;
        double ddxminus, ddyminus, ddzminus = 0.0;
        if (!GetSDF(posf + Eigen::Vector3f(voxelResolutionMeters, 0, 0), &ddxplus)) return false;
        if (!GetSDF(posf + Eigen::Vector3f(0, voxelResolutionMeters, 0), &ddyplus)) return false;
        if (!GetSDF(posf + Eigen::Vector3f(0, 0, voxelResolutionMeters), &ddzplus)) return false;
        if (!GetSDF(posf - Eigen::Vector3f(voxelResolutionMeters, 0, 0), &ddxminus)) return false;
        if (!GetSDF(posf - Eigen::Vector3f(0, voxelResolutionMeters, 0), &ddyminus)) return false;
        if (!GetSDF(posf - Eigen::Vector3f(0, 0, voxelResolutionMeters), &ddzminus)) return false;

        *grad = Eigen::Vector3f(ddxplus - ddxminus, ddyplus - ddyminus, ddzplus - ddzminus);
        grad->normalize();
        return true;
    }

    bool ChunkManager::GetSDF(const Eigen::Vector3f& posf, double* dist)
    {
        chisel::ChunkPtr chunk = GetChunkAt(posf);
        if(chunk)
        {
            Eigen::Vector3f relativePos = posf - chunk->GetOrigin();
            Eigen::Vector3i coords = chunk->GetVoxelCoords(relativePos);
            chisel::VoxelID id = chunk->GetVoxelID(coords);
            if(id >= 0 && id < chunk->GetTotalNumVoxels())
            {
                const chisel::DistVoxel& voxel = chunk->GetDistVoxel(id);
                if(voxel.GetWeight() > 1e-12)
                {
                    *dist = voxel.GetSDF();
                    return true;
                }
            }
            return false;
        }
        else
        {
            return false;
        }
    }

    // LOCAL MODIFICATION: this function was dimensionally wrong in two places and
    // could not have produced a correct colour.
    //
    // Upstream computed the eight corner indices as `floor(x / resolution)`, which
    // is a VOXEL INDEX, and then passed them straight to GetColorVoxel, which
    // takes a position in METRES. At 4 cm voxels that looks up a point 25 times
    // further from the origin than the one asked for. The eight lookups therefore
    // almost always landed in chunks that do not exist, the null test below fired,
    // and every call fell through to the nearest-neighbour fallback -- so the
    // trilinear path was dead code that, on the rare frame all eight indices
    // happened to hit allocated chunks, returned the colour of an unrelated corner
    // of the room.
    //
    // The interpolation weights had the same fault: `(x - x_0) / (x_1 - x_0)` is
    // metres minus an index over one, so even with the lookups fixed the blend
    // would have been meaningless.
    //
    // The grid is also not the one upstream indexed. A voxel's colour belongs at
    // its CENTROID, which CacheCentroids places at (index + 0.5) * resolution, and
    // marching cubes interpolates its vertices along edges of that same centroid
    // grid. So the cell containing a mesh vertex has lower corner
    // floor(p / resolution - 0.5), not floor(p / resolution). Getting this wrong
    // is a half-voxel bias, 2 cm here, which shifts every colour toward one corner
    // of the room.
    Vec3 ChunkManager::InterpolateColor(const Vec3& colorPos, float* weightOut)
    {
        // Continuous coordinates on the centroid grid: integer values land
        // exactly on voxel centres.
        const float gx = colorPos(0) / voxelResolutionMeters - 0.5f;
        const float gy = colorPos(1) / voxelResolutionMeters - 0.5f;
        const float gz = colorPos(2) / voxelResolutionMeters - 0.5f;

        const int x_0 = static_cast<int>(std::floor(gx));
        const int y_0 = static_cast<int>(std::floor(gy));
        const int z_0 = static_cast<int>(std::floor(gz));
        const int x_1 = x_0 + 1;
        const int y_1 = y_0 + 1;
        const int z_1 = z_0 + 1;

        // Back to metres, at the centroid of each of the eight surrounding
        // voxels, which is what GetColorVoxel actually wants.
        const auto centre = [this](int i, int j, int k) {
            return Vec3((static_cast<float>(i) + 0.5f) * voxelResolutionMeters,
                        (static_cast<float>(j) + 0.5f) * voxelResolutionMeters,
                        (static_cast<float>(k) + 0.5f) * voxelResolutionMeters);
        };

        const ColorVoxel* v_000 = GetColorVoxel(centre(x_0, y_0, z_0));
        const ColorVoxel* v_001 = GetColorVoxel(centre(x_0, y_0, z_1));
        const ColorVoxel* v_011 = GetColorVoxel(centre(x_0, y_1, z_1));
        const ColorVoxel* v_111 = GetColorVoxel(centre(x_1, y_1, z_1));
        const ColorVoxel* v_110 = GetColorVoxel(centre(x_1, y_1, z_0));
        const ColorVoxel* v_100 = GetColorVoxel(centre(x_1, y_0, z_0));
        const ColorVoxel* v_010 = GetColorVoxel(centre(x_0, y_1, z_0));
        const ColorVoxel* v_101 = GetColorVoxel(centre(x_1, y_0, z_1));

        // Weights normalise against the ceiling the integrator was given, so a
        // voxel at its ceiling reads exactly 1.0. Falling back to 255 keeps this
        // meaningful when no ceiling was set, which is upstream's unbounded mean.
        const float weightScale =
                1.0f / static_cast<float>(colorMaxWeight > 0 ? colorMaxWeight : 255);

        if(!v_000 || !v_001 || !v_011 || !v_111 || !v_110 || !v_100 || !v_010 || !v_101)
        {
            // At least one neighbour is outside any allocated chunk, which is the
            // ordinary case for a vertex on the outer face of the reconstruction.
            // Nearest neighbour at the position asked for, which needs only the
            // one chunk that vertex sits in.
            const ColorVoxel* nearest = GetColorVoxel(colorPos);
            if (nearest == nullptr)
            {
                if (weightOut) *weightOut = 0.0f;
                return Vec3(0, 0, 0);
            }
            if (weightOut) *weightOut = static_cast<float>(nearest->GetWeight()) * weightScale;
            return Vec3(static_cast<float>(nearest->GetRed()) / 255.0f,
                        static_cast<float>(nearest->GetGreen()) / 255.0f,
                        static_cast<float>(nearest->GetBlue()) / 255.0f);
        }

        const float xd = gx - static_cast<float>(x_0);
        const float yd = gy - static_cast<float>(y_0);
        const float zd = gz - static_cast<float>(z_0);

        const ColorVoxel* corners[8] = {v_000, v_100, v_010, v_110, v_001, v_101, v_011, v_111};
        const float coefficients[8] = {
                (1 - xd) * (1 - yd) * (1 - zd), xd * (1 - yd) * (1 - zd),
                (1 - xd) * yd * (1 - zd),       xd * yd * (1 - zd),
                (1 - xd) * (1 - yd) * zd,       xd * (1 - yd) * zd,
                (1 - xd) * yd * zd,             xd * yd * zd,
        };

        // LOCAL MODIFICATION: weighted by each corner's own colour weight, not a
        // plain blend of the eight RGB triples.
        //
        // The plain blend has a specific failure and it is not subtle. A voxel
        // that has never been seen in colour holds (0, 0, 0), which is
        // indistinguishable from black, so blending it in drags the result toward
        // black in proportion to how close the vertex is to it. That happens
        // wherever colour coverage is patchy against geometry coverage -- around
        // any voxel integrated on a frame whose camera image had not arrived yet,
        // and along the whole leading edge of a sweep -- and it reads as dark
        // fringing that looks like bad lighting or bad normals.
        //
        // Weighting by the colour weight makes an unobserved corner contribute
        // NOTHING rather than contributing black, which is what "no evidence"
        // means. The denominator is the trilinear interpolation of the weights
        // themselves, because the coefficients sum to one, so it doubles as the
        // confidence this function reports.
        float totalWeight = 0.0f;
        float red = 0.0f;
        float green = 0.0f;
        float blue = 0.0f;
        for (int corner = 0; corner < 8; corner++)
        {
            const float weight =
                    coefficients[corner] * static_cast<float>(corners[corner]->GetWeight());
            if (weight <= 0.0f)
            {
                continue;
            }
            totalWeight += weight;
            red += weight * static_cast<float>(corners[corner]->GetRed());
            green += weight * static_cast<float>(corners[corner]->GetGreen());
            blue += weight * static_cast<float>(corners[corner]->GetBlue());
        }

        // Every corner exists as geometry but none has been seen in colour. The
        // renderer reads the zero weight and draws its flat colour.
        if (totalWeight <= 0.0f)
        {
            if (weightOut) *weightOut = 0.0f;
            return Vec3(0, 0, 0);
        }

        // Reported before the division below, so it stays an interpolated colour
        // weight rather than a normalisation factor. A fade here is what makes
        // the boundary of a coloured region a gradient in the shader instead of
        // a hard edge.
        if (weightOut) *weightOut = totalWeight * weightScale;

        const float normalise = 1.0f / (totalWeight * 255.0f);
        return Vec3(red * normalise, green * normalise, blue * normalise);
    }

    const DistVoxel* ChunkManager::GetDistanceVoxel(const Vec3& pos)
    {
        ChunkPtr chunk = GetChunkAt(pos);

        if(chunk.get())
        {
            Vec3 rel = (pos - chunk->GetOrigin());
            return &(chunk->GetDistVoxel(chunk->GetVoxelID(rel)));
        }
        else return nullptr;
    }

    const ColorVoxel* ChunkManager::GetColorVoxel(const Vec3& pos)
    {
        ChunkPtr chunk = GetChunkAt(pos);

        if(chunk.get())
        {
            Vec3 rel = (pos - chunk->GetOrigin());
            const VoxelID& id = chunk->GetVoxelID(rel);
            if (id >= 0 && id < chunk->GetTotalNumVoxels())
            {
                return &(chunk->GetColorVoxel(id));
            }
            else
            {
                return nullptr;
            }
        }
        else return nullptr;
    }


    void ChunkManager::ComputeNormalsFromGradients(Mesh* mesh)
    {
        assert(mesh != nullptr);
        double dist;
        Vec3 grad;
        for (size_t i = 0; i < mesh->vertices.size(); i++)
        {
            const Vec3& vertex = mesh->vertices.at(i);
            if(GetSDFAndGradient(vertex, &dist, &grad))
            {
                float mag = grad.norm();
                if(mag> 1e-12)
                {
                    mesh->normals[i] = grad * (1.0f / mag);
                }
            }
        }
    }

    void ChunkManager::ColorizeMesh(Mesh* mesh)
    {
        assert(mesh != nullptr);

        mesh->colors.clear();
        mesh->colors.resize(mesh->vertices.size());
        // LOCAL MODIFICATION: the confidence lane, filled in step with the colour
        // so the two can never be different lengths. See Mesh::colorWeights.
        mesh->colorWeights.clear();
        mesh->colorWeights.resize(mesh->vertices.size());
        for (size_t i = 0; i < mesh->vertices.size(); i++)
        {
            const Vec3& vertex = mesh->vertices.at(i);
            mesh->colors[i] = InterpolateColor(vertex, &mesh->colorWeights[i]);
        }
    }


    void ChunkManager::PrintMemoryStatistics()
    {
        float bigFloat = std::numeric_limits<float>::max();

        chisel::AABB totalBounds;
        totalBounds.min = chisel::Vec3(bigFloat, bigFloat, bigFloat);
        totalBounds.max = chisel::Vec3(-bigFloat, -bigFloat, -bigFloat);

        ChunkStatistics stats;
        stats.numKnownInside = 0;
        stats.numKnownOutside = 0;
        stats.numUnknown = 0;
        stats.totalWeight = 0.0f;
        for (const std::pair<ChunkID, ChunkPtr>& chunk : chunks)
        {
            AABB bounds = chunk.second->ComputeBoundingBox();
            for (int i = 0; i < 3; i++)
            {
                totalBounds.min(i) = std::min(totalBounds.min(i), bounds.min(i));
                totalBounds.max(i) = std::max(totalBounds.max(i), bounds.max(i));
            }

            chunk.second->ComputeStatistics(&stats);
        }


        Vec3 ext = totalBounds.GetExtents();
        Vec3 numVoxels = ext * 2 / voxelResolutionMeters;
        float totalNum = numVoxels(0) * numVoxels(1) * numVoxels(2);

        float maxMemory = totalNum * sizeof(DistVoxel) / 1000000.0f;

        size_t currentNum = chunks.size() * (chunkSize(0) * chunkSize(1) * chunkSize(2));
        float currentMemory = currentNum * sizeof(DistVoxel) / 1000000.0f;

        printf("Num Unknown: %lu, Num KnownIn: %lu, Num KnownOut: %lu Weight: %f\n", stats.numUnknown, stats.numKnownInside, stats.numKnownOutside, stats.totalWeight);
        printf("Bounds: %f %f %f %f %f %f\n", totalBounds.min.x(), totalBounds.min.y(), totalBounds.min.z(), totalBounds.max.x(), totalBounds.max.y(), totalBounds.max.z());
        printf("Theoretical max (MB): %f, Current (MB): %f\n", maxMemory, currentMemory);

    }

} // namespace chisel 
