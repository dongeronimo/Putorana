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

#ifndef MARCHINGCUBES_H_
#define MARCHINGCUBES_H_

#include <cstdint>
#include <unordered_map>
#include <open_chisel/geometry/Geometry.h>
#include <open_chisel/mesh/Mesh.h>

namespace chisel
{

    typedef std::vector<Mat3x3, Eigen::aligned_allocator<Mat3x3> > TriangleVector;
    class MarchingCubes
    {
        public:
            static int triangleTable[256][16];
            static int edgeIndexPairs[12][2];

            // LOCAL MODIFICATION: the eight cube corner offsets, mirrored here
            // from ChunkManager's cubeIndexOffsets.
            //
            // They have to agree, and nothing in the compiler can check that.
            // MeshCube needs them to work out WHICH grid edge produced a vertex,
            // which is what lets it share one vertex between the cubes that meet
            // on that edge; ChunkManager needs the same offsets to fetch the
            // corner voxels. If the two ever drift apart the mesh will still
            // build and will silently weld the wrong vertices together.
            static int cubeCornerOffsets[8][3];

            /**
             * A grid edge's identity, as one integer.
             *
             * An edge is named by the LOWER of the two corners it spans plus the
             * axis it runs along, in the chunk's voxel coordinates. That naming
             * is what makes deduplication exact: the two cubes that share an edge
             * arrive at the same (corner, axis) pair by construction, whatever
             * order they happen to visit the corners in.
             *
             * The alternative -- hashing the interpolated POSITION -- looks
             * simpler and is not reliable. Two neighbouring cubes number their
             * corners differently, so one computes a + (b-a)*t and the other
             * b + (a-b)*t'. Those are equal in arithmetic and not equal in
             * float, so position hashing needs an epsilon, and an epsilon that
             * is slightly wrong welds vertices that should be apart or leaves
             * seams that should be closed.
             *
             * 20 bits per axis, which is 1,048,575 voxels -- a chunk is 16.
             * */
            static inline uint64_t EdgeKey(const Eigen::Vector3i& base, int axis)
            {
                return (static_cast<uint64_t>(base.x()) << 42) |
                       (static_cast<uint64_t>(base.y()) << 22) |
                       (static_cast<uint64_t>(base.z()) << 2) |
                       static_cast<uint64_t>(axis);
            }

            /** Which grid edge of `cubeIndex` the cube-local edge `edge` is. */
            static inline uint64_t EdgeKeyFor(const Eigen::Vector3i& cubeIndex, int edge)
            {
                const int* pair = edgeIndexPairs[edge];
                const Eigen::Vector3i a(cubeCornerOffsets[pair[0]][0], cubeCornerOffsets[pair[0]][1],
                                        cubeCornerOffsets[pair[0]][2]);
                const Eigen::Vector3i b(cubeCornerOffsets[pair[1]][0], cubeCornerOffsets[pair[1]][1],
                                        cubeCornerOffsets[pair[1]][2]);
                // Derived from the two tables rather than written out as a third
                // one. A hand-transcribed edge->(corner, axis) table is exactly
                // the kind of thing that is wrong in one entry and produces a
                // single welded seam nobody finds.
                int axis = 0;
                for (int k = 0; k < 3; ++k)
                {
                    if (a[k] != b[k]) axis = k;
                }
                return EdgeKey(cubeIndex + a.cwiseMin(b), axis);
            }

            MarchingCubes();
            ~MarchingCubes();


            static void MeshCube(const Eigen::Matrix<float, 3, 8>& vertex_coordinates, const Eigen::Matrix<float, 8, 1>& vertexSDF, TriangleVector* triangles)
            {
                assert(triangles != nullptr);

                const int index = CalculateVertexConfiguration(vertexSDF);

                Eigen::Matrix<float, 3, 12> edgeCoords;
                InterpolateEdgeVertices(vertex_coordinates, vertexSDF, &edgeCoords);

                const int* table_row = triangleTable[index];

                int edgeIDX = 0;
                int tableCol = 0;
                while ((edgeIDX = table_row[tableCol]) != -1)
                {
                    Eigen::Matrix3f triangle;
                    triangle.col(0) = edgeCoords.col(edgeIDX);
                    edgeIDX = table_row[tableCol + 1];
                    triangle.col(1) = edgeCoords.col(edgeIDX);
                    edgeIDX = table_row[tableCol + 2];
                    triangle.col(2) = edgeCoords.col(edgeIDX);
                    triangles->push_back(triangle);
                    tableCol += 3;
                }
            }

            static void MeshCube(const Eigen::Matrix<float, 3, 8>& vertexCoords, const Eigen::Matrix<float, 8, 1>& vertexSDF, VertIndex* nextIDX, Mesh* mesh)
            {
                assert(nextIDX != nullptr);
                assert(mesh != nullptr);
                const int index = CalculateVertexConfiguration(vertexSDF);

                Eigen::Matrix<float, 3, 12> edge_vertex_coordinates;
                InterpolateEdgeVertices(vertexCoords, vertexSDF, &edge_vertex_coordinates);

                const int* table_row = triangleTable[index];

                int table_col = 0;
                while (table_row[table_col] != -1)
                {
                    mesh->vertices.emplace_back(edge_vertex_coordinates.col(table_row[table_col + 2]));
                    mesh->vertices.emplace_back(edge_vertex_coordinates.col(table_row[table_col + 1]));
                    mesh->vertices.emplace_back(edge_vertex_coordinates.col(table_row[table_col]));
                    mesh->indices.push_back(*nextIDX);
                    mesh->indices.push_back((*nextIDX) + 1);
                    mesh->indices.push_back((*nextIDX) + 2);
                    const Eigen::Vector3f& p0 = mesh->vertices[*nextIDX];
                    const Eigen::Vector3f& p1 = mesh->vertices[*nextIDX + 1];
                    const Eigen::Vector3f& p2 = mesh->vertices[*nextIDX + 2];
                    Eigen::Vector3f px = (p1 - p0);
                    Eigen::Vector3f py = (p2 - p0);
                    Eigen::Vector3f n = px.cross(py).normalized();
                    mesh->normals.push_back(n);
                    mesh->normals.push_back(n);
                    mesh->normals.push_back(n);
                    *nextIDX += 3;
                    table_col += 3;
                }
            }

            /**
             * LOCAL MODIFICATION: the deduplicating MeshCube, and the one this
             * project uses. The overload above is upstream's and is left alone.
             *
             * Upstream emits a triangle SOUP: three brand new vertices per
             * triangle, and indices that are literally 0,1,2,3,4,5... The index
             * buffer is the identity permutation -- it carries no information at
             * all, and a non-indexed draw would produce the same picture.
             *
             * That costs about six times more vertices than the surface has.
             * Every marching-cubes vertex sits on a grid edge, and each such edge
             * is shared by the four cubes around it; on a manifold triangle mesh
             * V is roughly F/2, while a soup stores 3F.
             *
             * Upstream had a reason: it writes the FACE normal to all three
             * vertices, and flat shading genuinely cannot share a vertex between
             * two faces. But the reason does not survive its own pipeline --
             * ChunkManager::ComputeNormalsFromGradients runs afterwards and
             * overwrites every normal with the SDF gradient at the vertex
             * POSITION. That is a pure function of where the vertex is, so two
             * copies of the same position always end up with the same normal.
             * The duplication buys nothing, and removing it is lossless.
             *
             * Face normals are still accumulated here, unnormalised and hence
             * area-weighted, as the fallback for vertices where the gradient
             * lookup fails. GenerateMesh normalises them before the gradient pass
             * overwrites what it can.
             * */
            static void MeshCube(const Eigen::Matrix<float, 3, 8>& vertexCoords,
                                 const Eigen::Matrix<float, 8, 1>& vertexSDF,
                                 const Eigen::Vector3i& cubeIndex,
                                 std::unordered_map<uint64_t, VertIndex>* edgeVertices,
                                 VertIndex* nextIDX, Mesh* mesh)
            {
                assert(edgeVertices != nullptr);
                assert(nextIDX != nullptr);
                assert(mesh != nullptr);

                const int index = CalculateVertexConfiguration(vertexSDF);

                Eigen::Matrix<float, 3, 12> edge_vertex_coordinates;
                InterpolateEdgeVertices(vertexCoords, vertexSDF, &edge_vertex_coordinates);

                const int* table_row = triangleTable[index];

                int table_col = 0;
                while (table_row[table_col] != -1)
                {
                    VertIndex triangle[3];
                    for (int corner = 0; corner < 3; ++corner)
                    {
                        // +2, +1, +0 -- upstream's order, kept exactly, because
                        // it is what makes the winding come out counter-clockwise
                        // and the pipeline culls back faces.
                        const int edge = table_row[table_col + (2 - corner)];
                        const uint64_t key = EdgeKeyFor(cubeIndex, edge);

                        const auto found = edgeVertices->find(key);
                        if (found != edgeVertices->end())
                        {
                            triangle[corner] = found->second;
                            continue;
                        }

                        triangle[corner] = *nextIDX;
                        mesh->vertices.emplace_back(edge_vertex_coordinates.col(edge));
                        mesh->normals.emplace_back(Vec3::Zero());
                        (*edgeVertices)[key] = *nextIDX;
                        ++(*nextIDX);
                    }

                    mesh->indices.push_back(triangle[0]);
                    mesh->indices.push_back(triangle[1]);
                    mesh->indices.push_back(triangle[2]);

                    const Vec3& p0 = mesh->vertices[triangle[0]];
                    const Vec3& p1 = mesh->vertices[triangle[1]];
                    const Vec3& p2 = mesh->vertices[triangle[2]];
                    // NOT normalised: the magnitude is twice the triangle's area,
                    // which weights big triangles more than slivers when several
                    // meet at one vertex. Normalising here would throw that away.
                    const Vec3 n = (p1 - p0).cross(p2 - p0);
                    mesh->normals[triangle[0]] += n;
                    mesh->normals[triangle[1]] += n;
                    mesh->normals[triangle[2]] += n;

                    table_col += 3;
                }
            }

            static int CalculateVertexConfiguration(const Eigen::Matrix<float, 8, 1>& vertexSDF)
            {
                return  (vertexSDF(0) < 0 ? (1<<0) : 0) |
                        (vertexSDF(1) < 0 ? (1<<1) : 0) |
                        (vertexSDF(2) < 0 ? (1<<2) : 0) |
                        (vertexSDF(3) < 0 ? (1<<3) : 0) |
                        (vertexSDF(4) < 0 ? (1<<4) : 0) |
                        (vertexSDF(5) < 0 ? (1<<5) : 0) |
                        (vertexSDF(6) < 0 ? (1<<6) : 0) |
                        (vertexSDF(7) < 0 ? (1<<7) : 0);
            }

            static void InterpolateEdgeVertices(const Eigen::Matrix<float, 3, 8>& vertexCoords, const Eigen::Matrix<float, 8, 1>& vertSDF, Eigen::Matrix<float, 3, 12>* edgeCoords)
            {
                assert(edgeCoords != nullptr);
                for (std::size_t i = 0; i < 12; ++i)
                {
                    const int* pairs = edgeIndexPairs[i];
                    const int edge0 = pairs[0];
                    const int edge1 = pairs[1];
                    // Only interpolate along edges where there is a zero crossing.
                    if ((vertSDF(edge0) < 0 && vertSDF(edge1) >= 0) || (vertSDF(edge0) >= 0 && vertSDF(edge1) < 0))
                        edgeCoords->col(i) = InterpolateVertex(vertexCoords.col(edge0), vertexCoords.col(edge1), vertSDF(edge0), vertSDF(edge1));
                }
            }

            // Performs linear interpolation on two cube corners to find the approximate
            // zero crossing (surface) value.
            static inline Vec3 InterpolateVertex(const Vec3& vertex1, const Vec3& vertex2, const float& sdf1, const float& sdf2)
            {
                const float minDiff = 1e-6;
                const float sdfDiff = sdf1 - sdf2;
                if (fabs(sdfDiff) < minDiff)
                {
                    return Vec3(vertex1 + 0.5 * vertex2);
                }
                const float t = sdf1 / sdfDiff;
                return Vec3(vertex1 + t * (vertex2 - vertex1));
            }
    };

} // namespace chisel 

#endif // MARCHINGCUBES_H_ 
