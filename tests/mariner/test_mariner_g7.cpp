/**************************************************************************/
/*  test_mariner_g7.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_g7)

#include "core/io/image.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "scene/resources/3d/mesh_tessellator.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/shader_language.h"
#include "servers/rendering/shader_types.h"

// Mariner engine patch G7: tessellation and displacement. BaseMaterial3D.displacement_* moves the vertices along their
// normals by a height map and shades the relief per pixel; MeshTessellator splits a mesh to a target edge length (Phong
// tessellation optional) and builds its LODs on the displaced shape. Images on lavapipe: engine/tests/g/displacement.gd.

namespace TestMarinerG7 {

static Error compile_spatial(const String &p_code, String *r_error) {
	ShaderLanguage sl;
	ShaderLanguage::ShaderCompileInfo info;
	info.functions = ShaderTypes::get_singleton()->get_functions(RSE::SHADER_SPATIAL);
	info.render_modes = ShaderTypes::get_singleton()->get_modes(RSE::SHADER_SPATIAL);
	info.stencil_modes = ShaderTypes::get_singleton()->get_stencil_modes(RSE::SHADER_SPATIAL);
	info.shader_types = ShaderTypes::get_singleton()->get_types();
	ERR_PRINT_OFF;
	const Error err = sl.compile(p_code, info);
	ERR_PRINT_ON;
	*r_error = sl.get_error_text();
	return err;
}

// A height map with a round bump of height 1 at its center.
static Ref<ImageTexture> bump_texture(int p_size) {
	Ref<Image> img = Image::create_empty(p_size, p_size, false, Image::FORMAT_RF);
	for (int y = 0; y < p_size; y++) {
		for (int x = 0; x < p_size; x++) {
			const Vector2 d = (Vector2(x, y) + Vector2(0.5, 0.5)) / p_size - Vector2(0.5, 0.5);
			img->set_pixel(x, y, Color(Math::exp(-d.length_squared() / (2.0 * 0.08 * 0.08)), 0, 0));
		}
	}
	return ImageTexture::create_from_image(img);
}

struct EdgeHasher {
	static uint32_t hash(const Pair<Vector3, Vector3> &p_edge) {
		return hash_murmur3_one_32(HashMapHasherDefault::hash(p_edge.first), HashMapHasherDefault::hash(p_edge.second));
	}
};

struct Edges {
	HashMap<Pair<Vector3, Vector3>, int, EdgeHasher> count; // By the corners' positions.
	int boundary = 0;
	int non_manifold = 0;
	float longest = 0.0;
	double area = 0.0;
};

static Edges count_edges(const Array &p_arrays, const PackedInt32Array &p_indices) {
	const PackedVector3Array v = p_arrays[Mesh::ARRAY_VERTEX];
	Edges e;
	for (int i = 0; i < p_indices.size(); i += 3) {
		const Vector3 a = v[p_indices[i]];
		const Vector3 b = v[p_indices[i + 1]];
		const Vector3 c = v[p_indices[i + 2]];
		e.area += 0.5 * (b - a).cross(c - a).length();
		for (int k = 0; k < 3; k++) {
			Vector3 p = v[p_indices[i + k]];
			Vector3 q = v[p_indices[i + (k + 1) % 3]];
			e.longest = MAX(e.longest, p.distance_to(q));
			if (q < p) {
				SWAP(p, q);
			}
			e.count[Pair<Vector3, Vector3>(p, q)]++;
		}
	}
	for (const KeyValue<Pair<Vector3, Vector3>, int> &E : e.count) {
		if (E.value == 1) {
			e.boundary++;
		} else if (E.value > 2) {
			e.non_manifold++;
		}
	}
	return e;
}

TEST_CASE("[Mariner][G7] BaseMaterial3D.displacement") {
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(material->get_displacement_scale() == doctest::Approx(0.1));
	CHECK(material->get_displacement_midlevel() == doctest::Approx(0.0));
	CHECK(material->is_displacement_normal_enabled());
	BaseMaterial3D::flush_changes();
	CHECK_FALSE(RS::get_singleton()->shader_get_code(material->get_shader_rid()).contains("texture_displacement"));

	material->set_feature(BaseMaterial3D::FEATURE_DISPLACEMENT, true);
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	BaseMaterial3D::flush_changes();
	String code = RS::get_singleton()->shader_get_code(material->get_shader_rid());
	CHECK(code.contains("uniform sampler2D texture_displacement"));
	CHECK(code.contains("VERTEX += normalize(NORMAL) * ((textureLod(texture_displacement, UV, displacement_texture_lod).r - displacement_midlevel) * displacement_scale);"));
	CHECK(code.contains("// Displacement Normal: Enabled"));
	// The displacement uses the scaled UV (after uv1_scale), as the fragment shader's base_uv.
	CHECK(code.find("UV = UV * uv1_scale.xy + uv1_offset.xy;") < code.find("VERTEX += normalize(NORMAL)"));
	String error;
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	material->set_displacement_normal(false);
	BaseMaterial3D::flush_changes();
	code = RS::get_singleton()->shader_get_code(material->get_shader_rid());
	CHECK(code.contains("VERTEX += normalize(NORMAL)"));
	CHECK_FALSE(code.contains("// Displacement Normal: Enabled"));
	CHECK_MESSAGE(compile_spatial(code, &error) == OK, error);

	// No UV for the vertex with the triplanar mapping, no model-view of its own with the billboards: no displacement.
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, true);
	BaseMaterial3D::flush_changes();
	CHECK_FALSE(RS::get_singleton()->shader_get_code(material->get_shader_rid()).contains("texture_displacement"));
	material->set_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR, false);
	material->set_billboard_mode(BaseMaterial3D::BILLBOARD_ENABLED);
	BaseMaterial3D::flush_changes();
	CHECK_FALSE(RS::get_singleton()->shader_get_code(material->get_shader_rid()).contains("texture_displacement"));

	material->set_displacement_midlevel(2.0);
	CHECK(material->get_displacement_midlevel() == doctest::Approx(1.0));
	CHECK(ClassDB::has_property("BaseMaterial3D", "displacement_texture"));
}

TEST_CASE("[Mariner][G7] MeshTessellator splits to the target edge length") {
	Ref<PlaneMesh> plane;
	plane.instantiate(); // 2 x 2 m, two triangles.
	Ref<MeshTessellator> tess;
	tess.instantiate();
	tess->set_target_edge_length(0.1);
	tess->set_generate_lods(false);
	Ref<ArrayMesh> mesh = tess->tessellate(plane);
	REQUIRE(mesh.is_valid());
	REQUIRE(mesh->get_surface_count() == 1);
	const Array arrays = mesh->surface_get_arrays(0);
	const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
	const Edges e = count_edges(arrays, indices);
	CHECK(e.longest <= 0.1 + 1e-5);
	CHECK(indices.size() / 3 > 2 * 400); // At least the area over the largest triangle of the target.
	CHECK(e.area == doctest::Approx(4.0).epsilon(1e-4));
	CHECK(e.non_manifold == 0);
	// No T-junctions: the inner edges are each shared by two triangles; the border is split into pieces of the square's.
	int border = 0;
	for (const KeyValue<Pair<Vector3, Vector3>, int> &E : e.count) {
		if (E.value == 1) {
			const Vector3 &p = E.key.first;
			const Vector3 &q = E.key.second;
			const bool on_border = (Math::is_equal_approx(Math::abs(p.x), 1.0f) && Math::is_equal_approx(p.x, q.x)) || (Math::is_equal_approx(Math::abs(p.z), 1.0f) && Math::is_equal_approx(p.z, q.z));
			border += on_border ? 1 : 0;
		}
	}
	CHECK(border == e.boundary);
	// The attributes follow the place: the plane's UV is its position, its normal up, its tangent along +X.
	const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array n = arrays[Mesh::ARRAY_NORMAL];
	const PackedVector2Array uv = arrays[Mesh::ARRAY_TEX_UV];
	const PackedFloat32Array t = arrays[Mesh::ARRAY_TANGENT];
	int wrong = 0;
	for (int i = 0; i < v.size(); i++) {
		// (The mesh's vectors went through the attribute compression: a little off.)
		wrong += uv[i].distance_to(Vector2(v[i].x * 0.5 + 0.5, v[i].z * 0.5 + 0.5)) < 1e-4 ? 0 : 1;
		wrong += n[i].distance_to(Vector3(0, 1, 0)) < 1e-3 ? 0 : 1;
		wrong += Vector3(t[i * 4], t[i * 4 + 1], t[i * 4 + 2]).distance_to(Vector3(1, 0, 0)) < 1e-3 && t[i * 4 + 3] == 1.0f ? 0 : 1;
	}
	CHECK(wrong == 0);
	// The winding stays: every triangle faces up (Godot's front faces are clockwise).
	int flipped = 0;
	for (int i = 0; i < indices.size(); i += 3) {
		const Vector3 face = (v[indices[i + 2]] - v[indices[i]]).cross(v[indices[i + 1]] - v[indices[i]]);
		flipped += face.y > 0.0 ? 0 : 1;
	}
	CHECK(flipped == 0);

	// The triangle limit stops the splitting.
	tess->set_target_edge_length(0.001);
	tess->set_max_triangles(1000);
	ERR_PRINT_OFF;
	mesh = tess->tessellate(plane);
	ERR_PRINT_ON;
	CHECK(PackedInt32Array(mesh->surface_get_arrays(0)[Mesh::ARRAY_INDEX]).size() / 3 <= 1000);
}

TEST_CASE("[Mariner][G7] MeshTessellator Phong tessellation") {
	Ref<SphereMesh> sphere;
	sphere.instantiate(); // Radius 0.5, coarse.
	sphere->set_radial_segments(8);
	sphere->set_rings(4);
	Ref<MeshTessellator> tess;
	tess.instantiate();
	tess->set_target_edge_length(0.04);
	tess->set_generate_lods(false);

	double flat_error = 0.0;
	double phong_error = 0.0;
	for (int phong = 0; phong < 2; phong++) {
		tess->set_phong_enabled(phong == 1);
		tess->set_phong_shape(0.75);
		Ref<ArrayMesh> mesh = tess->tessellate(sphere);
		const Array arrays = mesh->surface_get_arrays(0);
		const PackedInt32Array indices = arrays[Mesh::ARRAY_INDEX];
		const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
		double error = 0.0;
		for (int i = 0; i < v.size(); i++) {
			error += Math::abs(v[i].length() - 0.5);
		}
		(phong ? phong_error : flat_error) = error / v.size();
		// Closed: the UV seam's two sides split alike (no cracks), with Phong as without.
		const Edges e = count_edges(arrays, indices);
		CHECK_MESSAGE(e.boundary == 0, "phong ", phong);
		CHECK_MESSAGE(e.non_manifold == 0, "phong ", phong);
	}
	MESSAGE("mean distance to the sphere: flat ", flat_error, ", Phong ", phong_error);
	CHECK(phong_error < flat_error * 0.5);

	// Hard edges stay straight: a box's faces stay flat with Phong.
	Ref<BoxMesh> box;
	box.instantiate();
	tess->set_phong_enabled(true);
	tess->set_target_edge_length(0.1);
	Ref<ArrayMesh> mesh = tess->tessellate(box);
	const PackedVector3Array v = mesh->surface_get_arrays(0)[Mesh::ARRAY_VERTEX];
	int off = 0;
	for (int i = 0; i < v.size(); i++) {
		const float m = MAX(Math::abs(v[i].x), MAX(Math::abs(v[i].y), Math::abs(v[i].z)));
		off += Math::is_equal_approx(m, 0.5f) ? 0 : 1;
	}
	CHECK(off == 0);
}

TEST_CASE("[Mariner][G7] MeshTessellator LODs on the displaced shape") {
	Ref<PlaneMesh> plane;
	plane.instantiate();
	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_feature(BaseMaterial3D::FEATURE_DISPLACEMENT, true);
	material->set_texture(BaseMaterial3D::TEXTURE_DISPLACEMENT, bump_texture(64));
	material->set_displacement_scale(0.5);
	material->set_displacement_midlevel(0.0);

	Ref<MeshTessellator> tess;
	tess.instantiate();
	tess->set_target_edge_length(0.05);
	Ref<ArrayMesh> flat = tess->tessellate(plane);
	Ref<ArrayMesh> bumpy = tess->tessellate(plane, material);
	REQUIRE(flat.is_valid());
	REQUIRE(bumpy.is_valid());

	// The material's displacement read on the CPU: the bump's top in the middle, nothing at the border.
	const Array arrays = bumpy->surface_get_arrays(0);
	const PackedVector3Array shape = MeshTessellator::get_displaced_vertices(arrays, material);
	const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
	float top = 0.0;
	float border = 0.0;
	for (int i = 0; i < v.size(); i++) {
		CHECK(Math::is_equal_approx(shape[i].x, v[i].x));
		if (Vector2(v[i].x, v[i].z).length() < 0.01) {
			top = MAX(top, shape[i].y);
		}
		if (Math::abs(v[i].x) > 0.99) {
			border = MAX(border, shape[i].y);
		}
	}
	CHECK(top == doctest::Approx(0.5).epsilon(0.03));
	CHECK(border < 0.001);

	const Dictionary flat_lods = RS::get_singleton()->mesh_surface_get_lods(flat->get_rid(), 0);
	const Dictionary bumpy_lods = RS::get_singleton()->mesh_surface_get_lods(bumpy->get_rid(), 0);
	REQUIRE(flat_lods.size() > 0);
	REQUIRE(bumpy_lods.size() > 0);
	// The first LOD: the smallest error.
	// The LODs keep their vertices on the bump: its share of the indices grows as they coarsen; the flat plane's LODs
	// have no reason to.
	const auto near_share = [&](const PackedInt32Array &p_indices) {
		int near_count = 0;
		for (int i = 0; i < p_indices.size(); i++) {
			const Vector3 &p = v[p_indices[i]];
			near_count += Vector2(p.x, p.z).length() < 0.3 ? 1 : 0;
		}
		return float(near_count) / MAX(1, p_indices.size());
	};
	const auto best_share = [&](const Dictionary &p_lods, const String &p_what) {
		float best = 0.0;
		Array keys = p_lods.keys();
		keys.sort();
		for (int i = 0; i < keys.size(); i++) {
			const PackedInt32Array lod = p_lods[keys[i]];
			MESSAGE(p_what, " LOD ", i, ": ", lod.size() / 3, " triangles, bump share ", near_share(lod));
			best = MAX(best, near_share(lod));
		}
		return best;
	};
	const float full_share = near_share(arrays[Mesh::ARRAY_INDEX]);
	const float bumpy_share = best_share(bumpy_lods, "displaced");
	const float flat_share = best_share(flat_lods, "flat");
	MESSAGE("bump share of the full mesh ", full_share);
	CHECK(bumpy_share > full_share * 3.0);
	CHECK(flat_share < full_share * 2.0);
	// The culling box holds the displaced surface.
	CHECK(bumpy->get_custom_aabb().get_end().y >= 0.5f - 1e-4f);
	CHECK(flat->get_custom_aabb() == AABB());
}

TEST_CASE("[Mariner][G7] MeshTessellator keeps blend shapes, skins and materials") {
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	arrays[Mesh::ARRAY_VERTEX] = PackedVector3Array({ Vector3(0, 0, 0), Vector3(1, 0, 0), Vector3(0, 0, 1) });
	arrays[Mesh::ARRAY_NORMAL] = PackedVector3Array({ Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0) });
	arrays[Mesh::ARRAY_BONES] = PackedInt32Array({ 0, 0, 0, 0, 1, 0, 0, 0, 2, 0, 0, 0 });
	arrays[Mesh::ARRAY_WEIGHTS] = PackedFloat32Array({ 1, 0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0 });
	Array shape;
	shape.resize(Mesh::ARRAY_MAX);
	shape[Mesh::ARRAY_VERTEX] = PackedVector3Array({ Vector3(0, 1, 0), Vector3(1, 0, 0), Vector3(0, 0, 1) });
	shape[Mesh::ARRAY_NORMAL] = arrays[Mesh::ARRAY_NORMAL];
	Ref<ArrayMesh> source;
	source.instantiate();
	source->add_blend_shape("lift");
	source->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, { shape });
	Ref<StandardMaterial3D> material;
	material.instantiate();
	source->surface_set_material(0, material);
	source->surface_set_name(0, "deck");

	Ref<MeshTessellator> tess;
	tess.instantiate();
	tess->set_target_edge_length(0.3);
	tess->set_generate_lods(false);
	Ref<ArrayMesh> mesh = tess->tessellate(source);
	REQUIRE(mesh.is_valid());
	CHECK(mesh->get_blend_shape_count() == 1);
	CHECK(mesh->surface_get_material(0) == material);
	CHECK(mesh->surface_get_name(0) == "deck");
	const Array out = mesh->surface_get_arrays(0);
	const PackedVector3Array v = out[Mesh::ARRAY_VERTEX];
	const PackedInt32Array bones = out[Mesh::ARRAY_BONES];
	const PackedFloat32Array weights = out[Mesh::ARRAY_WEIGHTS];
	const Array out_shape = mesh->surface_get_blend_shape_arrays(0)[0];
	const PackedVector3Array sv = out_shape[Mesh::ARRAY_VERTEX];
	REQUIRE(v.size() > 3);
	REQUIRE(sv.size() == v.size());
	REQUIRE(bones.size() == v.size() * 4);
	int wrong = 0;
	for (int i = 0; i < v.size(); i++) {
		// The barycentric place of a vertex: (1 - x - z, x, z) - the lift moves it by the first corner's weight.
		const float w0 = 1.0f - v[i].x - v[i].z;
		wrong += Math::is_equal_approx(sv[i].y - v[i].y, w0, 1e-4f) ? 0 : 1;
		float bone_weight[3] = { 0, 0, 0 };
		float sum = 0.0;
		for (int k = 0; k < 4; k++) {
			if (weights[i * 4 + k] > 0.0f) {
				bone_weight[bones[i * 4 + k]] += weights[i * 4 + k];
			}
			sum += weights[i * 4 + k];
		}
		wrong += Math::is_equal_approx(sum, 1.0f, 1e-4f) ? 0 : 1;
		wrong += Math::is_equal_approx(bone_weight[1], v[i].x, 1e-3f) && Math::is_equal_approx(bone_weight[2], v[i].z, 1e-3f) ? 0 : 1;
	}
	CHECK(wrong == 0);
	CHECK(ClassDB::class_exists("MeshTessellator"));
}

} // namespace TestMarinerG7
