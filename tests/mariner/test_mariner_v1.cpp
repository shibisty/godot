/**************************************************************************/
/*  test_mariner_v1.cpp                                                   */
/**************************************************************************/
/*                         This file is part of:                          */
/*                             GODOT ENGINE                               */
/*                        https://godotengine.org                         */
/**************************************************************************/
/* Copyright (c) 2014-present Godot Engine contributors (see AUTHORS.md). */
/* Copyright (c) 2007-2014 Juan Linietsky, Ariel Manzur.                  */
/*                                                                        */
/* Permission is hereby granted, free of charge, to any person obtaining  */
/* a copy of this software and associated documentation files (the        */
/* "Software"), to deal in the Software without restriction, including    */
/* without limitation the rights to use, copy, modify, merge, publish,    */
/* distribute, sublicense, and/or sell copies of the Software, and to     */
/* permit persons to whom the Software is furnished to do so, subject to  */
/* the following conditions:                                              */
/*                                                                        */
/* The above copyright notice and this permission notice shall be         */
/* included in all copies or substantial portions of the Software.        */
/*                                                                        */
/* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND,        */
/* EXPRESS OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF     */
/* MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. */
/* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY   */
/* CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT,   */
/* TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE      */
/* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                 */
/**************************************************************************/

#include "tests/test_macros.h"

TEST_FORCE_LINK(test_mariner_v1)

#include "core/object/class_db.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/mesh.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch V1: cluster LOD (the DAG of Nanite) built with meshoptimizer. Here: the structure of the DAG
// on a dense sphere (cluster sizes, levels, nested LOD spheres, monotonic errors), cuts at any error threshold that
// cover the surface once (area, triangle counts), and the data through ArrayMesh (build, save / load).

namespace TestMarinerV1 {

static Array dense_sphere(int p_segments, int p_rings) {
	Ref<SphereMesh> sphere;
	sphere.instantiate();
	sphere->set_radius(1.0);
	sphere->set_height(2.0);
	sphere->set_radial_segments(p_segments);
	sphere->set_rings(p_rings);
	return sphere->get_mesh_arrays();
}

static double area_of(const Vector<Vector3> &p_positions, const uint32_t *p_indices, uint32_t p_count) {
	double a = 0.0;
	for (uint32_t i = 0; i + 2 < p_count; i += 3) {
		const Vector3 &p0 = p_positions[p_indices[i]];
		const Vector3 &p1 = p_positions[p_indices[i + 1]];
		const Vector3 &p2 = p_positions[p_indices[i + 2]];
		a += 0.5 * (p1 - p0).cross(p2 - p0).length();
	}
	return a;
}

TEST_CASE("[Mariner][V1] Cluster DAG of a dense sphere: sizes, levels, nested spheres, monotonic errors") {
	const Array arrays = dense_sphere(128, 64);
	const Vector<Vector3> positions = arrays[Mesh::ARRAY_VERTEX];
	const Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
	Vector<uint8_t> index_data;
	Vector<uint8_t> cluster_data;
	int levels = 0;
	REQUIRE(SurfaceTool::build_cluster_lod(positions, arrays[Mesh::ARRAY_NORMAL], indices, index_data, cluster_data, &levels) == OK);
	const int count = cluster_data.size() / int(sizeof(SurfaceTool::ClusterLOD));
	const SurfaceTool::ClusterLOD *c = (const SurfaceTool::ClusterLOD *)cluster_data.ptr();
	const uint32_t total_indices = index_data.size() / 4;
	CHECK(levels >= 5);
	CHECK(count > indices.size() / 3 / SurfaceTool::CLUSTER_TRIANGLES);

	int level0 = 0;
	int roots = 0;
	uint32_t level0_indices = 0;
	bool sizes_ok = true;
	bool errors_ok = true;
	bool spheres_ok = true;
	bool ranges_ok = true;
	for (int i = 0; i < count; i++) {
		sizes_ok = sizes_ok && c[i].index_count > 0 && c[i].index_count % 3 == 0 && c[i].index_count <= SurfaceTool::CLUSTER_TRIANGLES * 3;
		ranges_ok = ranges_ok && c[i].index_offset + c[i].index_count <= total_indices;
		errors_ok = errors_ok && c[i].self_error <= c[i].parent_error;
		if (c[i].self_error == 0.0f) {
			level0++;
			level0_indices += c[i].index_count;
		}
		if (c[i].parent_error == FLT_MAX) {
			roots++;
		} else {
			// The replacing group's sphere contains this cluster's LOD sphere (screen errors stay monotonic).
			const Vector3 a(c[i].self_sphere[0], c[i].self_sphere[1], c[i].self_sphere[2]);
			const Vector3 b(c[i].parent_sphere[0], c[i].parent_sphere[1], c[i].parent_sphere[2]);
			spheres_ok = spheres_ok && a.distance_to(b) + c[i].self_sphere[3] <= c[i].parent_sphere[3] * 1.0001f + 1e-5f;
		}
	}
	CHECK(sizes_ok);
	CHECK(ranges_ok);
	CHECK(errors_ok);
	CHECK(spheres_ok);
	CHECK(level0_indices == uint32_t(indices.size())); // Full detail: every triangle once.
	CHECK(roots >= 1);
	CHECK(roots * 10 < level0);
}

TEST_CASE("[Mariner][V1] Cuts of the cluster DAG cover the surface once at any error") {
	const Array arrays = dense_sphere(128, 64);
	const Vector<Vector3> positions = arrays[Mesh::ARRAY_VERTEX];
	const Vector<int> indices = arrays[Mesh::ARRAY_INDEX];
	Vector<uint8_t> index_data;
	Vector<uint8_t> cluster_data;
	REQUIRE(SurfaceTool::build_cluster_lod(positions, arrays[Mesh::ARRAY_NORMAL], indices, index_data, cluster_data) == OK);
	const int count = cluster_data.size() / int(sizeof(SurfaceTool::ClusterLOD));
	const SurfaceTool::ClusterLOD *c = (const SurfaceTool::ClusterLOD *)cluster_data.ptr();
	const uint32_t *idx = (const uint32_t *)index_data.ptr();

	LocalVector<uint32_t> full;
	for (int i : indices) {
		full.push_back(i);
	}
	const double full_area = area_of(positions, full.ptr(), full.size());
	uint32_t previous_triangles = UINT32_MAX;
	const float thresholds[] = { 0.0f, 0.001f, 0.004f, 0.01f, 0.03f, 0.1f, 1e6f };
	for (float t : thresholds) {
		double area = 0.0;
		uint32_t triangles = 0;
		for (int i = 0; i < count; i++) {
			if (c[i].self_error <= t && t < c[i].parent_error) {
				area += area_of(positions, idx + c[i].index_offset, c[i].index_count);
				triangles += c[i].index_count / 3;
			}
		}
		INFO("threshold ", t, ": ", triangles, " triangles, area ", area, " of ", full_area);
		CHECK(triangles > 0);
		CHECK(triangles <= previous_triangles); // Coarser with the threshold.
		// One cover of the sphere: the area of the (simplified, slightly smaller) surface, not twice and not half.
		CHECK(area > full_area * 0.85);
		CHECK(area < full_area * 1.01);
		if (t == 0.0f) {
			CHECK(triangles == uint32_t(indices.size() / 3));
			CHECK(area == doctest::Approx(full_area));
		}
		previous_triangles = triangles;
	}
	CHECK(previous_triangles * 10 < uint32_t(indices.size() / 3)); // The coarsest cut is much smaller.
}

TEST_CASE("[Mariner][V1] ArrayMesh: build_cluster_lod, save and load") {
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, dense_sphere(64, 32));
	Array lines;
	lines.resize(Mesh::ARRAY_MAX);
	lines[Mesh::ARRAY_VERTEX] = PackedVector3Array({ Vector3(), Vector3(1, 0, 0) });
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_LINES, lines);
	CHECK(mesh->surface_get_cluster_count(0) == 0);
	CHECK(mesh->build_cluster_lod() == OK);
	const int clusters = mesh->surface_get_cluster_count(0);
	CHECK(clusters > 0);
	CHECK(mesh->surface_get_cluster_count(1) == 0); // Lines: nothing.
	CHECK(mesh->get_surface_count() == 2);

	// Through the surfaces property (as saved in a resource).
	Ref<ArrayMesh> copy = mesh->duplicate();
	REQUIRE(copy.is_valid());
	CHECK(copy->surface_get_cluster_count(0) == clusters);
	const RenderingServerTypes::SurfaceData a = RS::get_singleton()->mesh_get_surface(mesh->get_rid(), 0);
	const RenderingServerTypes::SurfaceData b = RS::get_singleton()->mesh_get_surface(copy->get_rid(), 0);
	CHECK(a.cluster_index_data == b.cluster_index_data);
	CHECK(a.cluster_data == b.cluster_data);
	CHECK(ClassDB::has_method("ArrayMesh", "build_cluster_lod"));
}

} // namespace TestMarinerV1
