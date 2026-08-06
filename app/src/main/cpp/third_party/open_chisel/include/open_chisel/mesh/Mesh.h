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

#ifndef MESH_H_
#define MESH_H_

#include <memory>
#include <vector>
#include <open_chisel/geometry/Geometry.h>

namespace chisel
{
    typedef size_t VertIndex;
    typedef std::vector<VertIndex> VertIndexList;
    class Mesh
    {
        public:
            EIGEN_MAKE_ALIGNED_OPERATOR_NEW;
            Mesh();
            ~Mesh();

            inline bool HasVertices() const { return !vertices.empty(); }
            inline bool HasNormals() const { return !normals.empty(); }
            inline bool HasColors() const { return !colors.empty(); }
            inline bool HasIndices() const { return !indices.empty(); }

            inline void Clear()
            {
                vertices.clear();
                normals.clear();
                colors.clear();
                colorWeights.clear();
                indices.clear();
            }

            Vec3List vertices;
            VertIndexList indices;
            Vec3List normals;
            Vec3List colors;

            // LOCAL ADDITION: how much colour evidence stands behind each entry
            // of `colors`, normalised to [0, 1].
            //
            // `colors` alone cannot answer that. A voxel nobody has seen in
            // colour interpolates to (0, 0, 0), and so does a black surface that
            // has been seen a hundred times. Telling them apart is what lets a
            // renderer fall back to a flat colour where the reconstruction has
            // geometry but no colour yet, instead of painting those regions
            // black and calling it data.
            //
            // Filled by ColorizeMesh alongside `colors`, so it is empty in
            // exactly the cases `colors` is.
            std::vector<float> colorWeights;

    };
    typedef std::shared_ptr<Mesh> MeshPtr;
    typedef std::shared_ptr<const Mesh> MeshConstPtr;

} // namespace chisel 

#endif // MESH_H_ 
