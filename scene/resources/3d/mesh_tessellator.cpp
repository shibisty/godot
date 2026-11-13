/**************************************************************************/
/*  mesh_tessellator.cpp                                                  */
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

#include "mesh_tessellator.h"

#include "core/io/image.h"
#include "core/object/class_db.h"
#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"
#include "scene/resources/3d/importer_mesh.h"
#include "scene/resources/material.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server_enums.h"

namespace {

// The material's displacement, read on the CPU as the vertex shader reads it (the first mipmap, bilinear).
struct DisplacementSampler {
	LocalVector<float> heights;
	int width = 0;
	int height = 0;
	bool repeat = true;
	Vector2 uv_scale = Vector2(1, 1);
	Vector2 uv_offset;
	float scale = 0.0;
	float midlevel = 0.0;

	bool setup(const Ref<Material> &p_material) {
		Ref<BaseMaterial3D> mat = p_material;
		if (mat.is_null() || !mat->get_feature(BaseMaterial3D::FEATURE_DISPLACEMENT)) {
			return false;
		}
		if (mat->get_billboard_mode() != BaseMaterial3D::BILLBOARD_DISABLED || mat->get_flag(BaseMaterial3D::FLAG_UV1_USE_TRIPLANAR)) {
			return false; // The material does not displace then.
		}
		Ref<Texture2D> tex = mat->get_texture(BaseMaterial3D::TEXTURE_DISPLACEMENT);
		if (tex.is_null()) {
			return false;
		}
		Ref<Image> img = tex->get_image();
		if (img.is_null() || img->is_empty()) {
			return false;
		}
		if (img->is_compressed()) {
			img = img->duplicate();
			ERR_FAIL_COND_V_MSG(img->decompress() != OK, false, "MeshTessellator: Cannot decompress the displacement texture.");
		}
		width = img->get_width();
		height = img->get_height();
		heights.resize(width * height);
		for (int y = 0; y < height; y++) {
			for (int x = 0; x < width; x++) {
				heights[y * width + x] = img->get_pixel(x, y).r;
			}
		}
		repeat = mat->get_flag(BaseMaterial3D::FLAG_USE_TEXTURE_REPEAT);
		const Vector3 s = mat->get_uv1_scale();
		const Vector3 o = mat->get_uv1_offset();
		uv_scale = Vector2(s.x, s.y);
		uv_offset = Vector2(o.x, o.y);
		scale = mat->get_displacement_scale();
		midlevel = mat->get_displacement_midlevel();
		return true;
	}

	float texel(int p_x, int p_y) const {
		if (repeat) {
			p_x = Math::posmod(p_x, width);
			p_y = Math::posmod(p_y, height);
		} else {
			p_x = CLAMP(p_x, 0, width - 1);
			p_y = CLAMP(p_y, 0, height - 1);
		}
		return heights[p_y * width + p_x];
	}

	// The offset along the normal at a mesh UV.
	float offset(const Vector2 &p_uv) const {
		const Vector2 uv = p_uv * uv_scale + uv_offset;
		const float fx = uv.x * width - 0.5f;
		const float fy = uv.y * height - 0.5f;
		const int x = (int)Math::floor(fx);
		const int y = (int)Math::floor(fy);
		const float tx = fx - x;
		const float ty = fy - y;
		const float top = Math::lerp(texel(x, y), texel(x + 1, y), tx);
		const float bottom = Math::lerp(texel(x, y + 1), texel(x + 1, y + 1), tx);
		return (Math::lerp(top, bottom, ty) - midlevel) * scale;
	}

	// How far the surface can move from the mesh.
	float reach() const {
		return MAX(Math::abs(scale * (1.0f - midlevel)), Math::abs(scale * midlevel));
	}
};

struct Triangle {
	uint32_t v[3];
	Vector3 bary[3]; // The corners in the original triangle.
	uint32_t original;
};

struct EdgeCandidate {
	float length;
	uint64_t key;
	bool operator<(const EdgeCandidate &p_other) const { return length > p_other.length; } // The longest first.
};

_FORCE_INLINE_ uint64_t edge_key(uint32_t p_a, uint32_t p_b) {
	return p_a < p_b ? ((uint64_t)p_a << 32) | p_b : ((uint64_t)p_b << 32) | p_a;
}

// A surface split until no edge is longer than the target.
class SurfaceTessellation {
	const PackedVector3Array &positions;
	const PackedVector3Array &normals;
	const PackedInt32Array &indices;
	const bool phong;
	const float phong_shape;
	LocalVector<float> smooth; // 0 for the vertices on a hard edge: Phong tessellation keeps those edges straight.

	HashMap<uint64_t, LocalVector<uint32_t>> edge_triangles;

	void _edge_add(uint32_t p_a, uint32_t p_b, uint32_t p_triangle) {
		edge_triangles[edge_key(p_a, p_b)].push_back(p_triangle);
	}

	void _edge_replace(uint32_t p_a, uint32_t p_b, uint32_t p_from, uint32_t p_to) {
		HashMap<uint64_t, LocalVector<uint32_t>>::Iterator E = edge_triangles.find(edge_key(p_a, p_b));
		ERR_FAIL_COND(!E);
		for (uint32_t &t : E->value) {
			if (t == p_from) {
				t = p_to;
				return;
			}
		}
	}

	void _split(uint64_t p_key) {
		HashMap<uint64_t, LocalVector<uint32_t>>::Iterator E = edge_triangles.find(p_key);
		if (!E) {
			return;
		}
		LocalVector<uint32_t> around;
		around = E->value;
		edge_triangles.remove(E);
		const uint32_t a = uint32_t(p_key >> 32);
		const uint32_t b = uint32_t(p_key & 0xFFFFFFFF);

		uint32_t m = UINT32_MAX;
		for (uint32_t t : around) {
			Triangle tri = triangles[t];
			int ia = -1;
			int ib = -1;
			for (int k = 0; k < 3; k++) {
				if (tri.v[k] == a) {
					ia = k;
				} else if (tri.v[k] == b) {
					ib = k;
				}
			}
			ERR_CONTINUE(ia < 0 || ib < 0);
			const int ic = 3 - ia - ib;
			const uint32_t c = tri.v[ic];
			const Vector3 bm = (tri.bary[ia] + tri.bary[ib]) * 0.5f;
			if (m == UINT32_MAX) {
				m = vertex_positions.size();
				vertex_original.push_back(tri.original);
				vertex_bary.push_back(bm);
				vertex_positions.push_back(point(tri.original, bm));
			}
			// (a, b, c) -> (a, m, c) and (m, b, c), in the same winding.
			Triangle second = tri;
			second.v[ia] = m;
			second.bary[ia] = bm;
			tri.v[ib] = m;
			tri.bary[ib] = bm;
			const uint32_t t2 = triangles.size();
			triangles[t] = tri;
			triangles.push_back(second);

			_edge_add(a, m, t);
			_edge_add(m, b, t2);
			_edge_add(m, c, t);
			_edge_add(m, c, t2);
			_edge_replace(b, c, t, t2);
		}
	}

public:
	LocalVector<Triangle> triangles;
	LocalVector<Vector3> vertex_positions; // All the vertices: the original ones, then the new.
	LocalVector<uint32_t> vertex_original; // The new vertices' original triangle...
	LocalVector<Vector3> vertex_bary; // ...and their place in it.
	uint32_t original_vertex_count = 0;
	bool truncated = false;

	Vector3 point(uint32_t p_original, const Vector3 &p_bary) const {
		const int i0 = indices[p_original * 3 + 0];
		const int i1 = indices[p_original * 3 + 1];
		const int i2 = indices[p_original * 3 + 2];
		const Vector3 p = positions[i0] * p_bary.x + positions[i1] * p_bary.y + positions[i2] * p_bary.z;
		if (!phong) {
			return p;
		}
		// Phong tessellation (Boubekeur, Alexa 2008): the point projected on the tangent planes of the corners.
		const Vector3 n0 = normals[i0].normalized();
		const Vector3 n1 = normals[i1].normalized();
		const Vector3 n2 = normals[i2].normalized();
		const Vector3 q = (p - n0 * (p - positions[i0]).dot(n0)) * p_bary.x + (p - n1 * (p - positions[i1]).dot(n1)) * p_bary.y + (p - n2 * (p - positions[i2]).dot(n2)) * p_bary.z;
		// Only the edge's own corners weigh on a point of an edge, so its neighbors agree on it.
		const float shape = phong_shape * (smooth[i0] * p_bary.x + smooth[i1] * p_bary.y + smooth[i2] * p_bary.z);
		return p.lerp(q, shape);
	}

	SurfaceTessellation(const PackedVector3Array &p_positions, const PackedVector3Array &p_normals, const PackedInt32Array &p_indices, bool p_phong, float p_phong_shape) :
			positions(p_positions), normals(p_normals), indices(p_indices), phong(p_phong && p_normals.size() == p_positions.size()), phong_shape(p_phong_shape) {
		original_vertex_count = positions.size();
		vertex_positions.resize(original_vertex_count);
		for (uint32_t i = 0; i < original_vertex_count; i++) {
			vertex_positions[i] = positions[i];
		}

		if (phong) {
			// A hard edge: two triangles meet at it with different normals (split vertices in the same places).
			smooth.resize(original_vertex_count);
			HashMap<Vector3, uint32_t> welded;
			LocalVector<uint32_t> place;
			place.resize(original_vertex_count);
			for (uint32_t i = 0; i < original_vertex_count; i++) {
				HashMap<Vector3, uint32_t>::Iterator E = welded.find(positions[i]);
				if (E) {
					place[i] = E->value;
				} else {
					place[i] = welded.size();
					welded.insert(positions[i], place[i]);
				}
			}
			LocalVector<bool> hard;
			hard.resize(welded.size());
			for (uint32_t i = 0; i < hard.size(); i++) {
				hard[i] = false;
			}
			HashMap<uint64_t, Pair<Vector3, Vector3>> edge_normals; // At the lower and the higher place.
			for (int t = 0; t + 2 < indices.size(); t += 3) {
				for (int k = 0; k < 3; k++) {
					int i = indices[t + k];
					int j = indices[t + (k + 1) % 3];
					if (place[i] > place[j]) {
						SWAP(i, j);
					}
					if (place[i] == place[j]) {
						continue;
					}
					const uint64_t key = edge_key(place[i], place[j]);
					const Vector3 ni = normals[i].normalized();
					const Vector3 nj = normals[j].normalized();
					HashMap<uint64_t, Pair<Vector3, Vector3>>::Iterator E = edge_normals.find(key);
					if (!E) {
						edge_normals.insert(key, Pair<Vector3, Vector3>(ni, nj));
					} else if (E->value.first.dot(ni) < 0.9999f || E->value.second.dot(nj) < 0.9999f) {
						hard[place[i]] = true;
						hard[place[j]] = true;
					}
				}
			}
			for (uint32_t i = 0; i < original_vertex_count; i++) {
				smooth[i] = hard[place[i]] ? 0.0f : 1.0f;
			}
		}

		// Triangles of no area (two corners in one place, like the fans at a UV sphere's poles) are left out: split, they
		// would only pile up.
		const uint32_t count = indices.size() / 3;
		triangles.reserve(count);
		for (uint32_t t = 0; t < count; t++) {
			Triangle tri;
			tri.original = t;
			for (int k = 0; k < 3; k++) {
				tri.v[k] = indices[t * 3 + k];
				tri.bary[k] = Vector3();
				tri.bary[k][k] = 1.0f;
			}
			if (positions[tri.v[0]] == positions[tri.v[1]] || positions[tri.v[1]] == positions[tri.v[2]] || positions[tri.v[2]] == positions[tri.v[0]]) {
				continue;
			}
			const uint32_t id = triangles.size();
			triangles.push_back(tri);
			for (int k = 0; k < 3; k++) {
				_edge_add(tri.v[k], tri.v[(k + 1) % 3], id);
			}
		}
	}

	void run(float p_target, int p_max_triangles) {
		while (true) {
			LocalVector<EdgeCandidate> candidates;
			for (const KeyValue<uint64_t, LocalVector<uint32_t>> &E : edge_triangles) {
				const float length = vertex_positions[uint32_t(E.key >> 32)].distance_to(vertex_positions[uint32_t(E.key & 0xFFFFFFFF)]);
				if (length > p_target) {
					candidates.push_back({ length, E.key });
				}
			}
			if (candidates.is_empty()) {
				return;
			}
			candidates.sort();
			for (const EdgeCandidate &c : candidates) {
				if ((int64_t)triangles.size() + 2 > p_max_triangles) {
					truncated = true;
					return;
				}
				_split(c.key);
			}
		}
	}
};

// A vertex attribute of the new vertices, from the corners of their original triangle.
template <typename T>
Vector<T> interpolate_attribute(const Vector<T> &p_values, const SurfaceTessellation &p_tess, const PackedInt32Array &p_indices) {
	if (p_values.size() != (int)p_tess.original_vertex_count) {
		return Vector<T>();
	}
	Vector<T> result = p_values;
	const uint32_t total = p_tess.vertex_positions.size();
	result.resize(total);
	T *w = result.ptrw();
	for (uint32_t i = p_tess.original_vertex_count; i < total; i++) {
		const uint32_t j = i - p_tess.original_vertex_count;
		const uint32_t o = p_tess.vertex_original[j];
		const Vector3 &b = p_tess.vertex_bary[j];
		w[i] = p_values[p_indices[o * 3 + 0]] * b.x + p_values[p_indices[o * 3 + 1]] * b.y + p_values[p_indices[o * 3 + 2]] * b.z;
	}
	return result;
}

// A packed attribute of p_width numbers per vertex: linear for floats, the nearest corner's for bytes.
template <typename T>
Vector<T> interpolate_packed(const Vector<T> &p_values, int p_width, bool p_linear, const SurfaceTessellation &p_tess, const PackedInt32Array &p_indices) {
	if (p_width <= 0 || p_values.size() != (int)p_tess.original_vertex_count * p_width) {
		return Vector<T>();
	}
	Vector<T> result = p_values;
	const uint32_t total = p_tess.vertex_positions.size();
	result.resize(total * p_width);
	T *w = result.ptrw();
	const T *r = p_values.ptr();
	for (uint32_t i = p_tess.original_vertex_count; i < total; i++) {
		const uint32_t j = i - p_tess.original_vertex_count;
		const uint32_t o = p_tess.vertex_original[j];
		const Vector3 &b = p_tess.vertex_bary[j];
		const int c0 = p_indices[o * 3 + 0];
		const int c1 = p_indices[o * 3 + 1];
		const int c2 = p_indices[o * 3 + 2];
		if (p_linear) {
			for (int k = 0; k < p_width; k++) {
				w[i * p_width + k] = T(r[c0 * p_width + k] * b.x + r[c1 * p_width + k] * b.y + r[c2 * p_width + k] * b.z);
			}
		} else {
			const int c = b.x >= b.y && b.x >= b.z ? c0 : (b.y >= b.z ? c1 : c2);
			for (int k = 0; k < p_width; k++) {
				w[i * p_width + k] = r[c * p_width + k];
			}
		}
	}
	return result;
}

Vector<int> surface_indices(const Array &p_arrays) {
	PackedInt32Array indices = p_arrays[Mesh::ARRAY_INDEX];
	if (indices.is_empty()) {
		const int count = PackedVector3Array(p_arrays[Mesh::ARRAY_VERTEX]).size();
		indices.resize(count - count % 3);
		int *w = indices.ptrw();
		for (int i = 0; i < indices.size(); i++) {
			w[i] = i;
		}
	}
	return indices;
}

} // namespace

PackedVector3Array MeshTessellator::get_displaced_vertices(const Array &p_arrays, const Ref<Material> &p_material) {
	ERR_FAIL_COND_V(p_arrays.size() != Mesh::ARRAY_MAX, PackedVector3Array());
	PackedVector3Array positions = p_arrays[Mesh::ARRAY_VERTEX];
	const PackedVector3Array normals = p_arrays[Mesh::ARRAY_NORMAL];
	const PackedVector2Array uvs = p_arrays[Mesh::ARRAY_TEX_UV];
	DisplacementSampler sampler;
	if (normals.size() != positions.size() || uvs.size() != positions.size() || !sampler.setup(p_material)) {
		return positions;
	}
	Vector3 *w = positions.ptrw();
	for (int i = 0; i < positions.size(); i++) {
		w[i] += normals[i].normalized() * sampler.offset(uvs[i]);
	}
	return positions;
}

float MeshTessellator::get_displacement_texture_lod(const Ref<Mesh> &p_mesh, const Ref<Material> &p_material, float p_edge_length) {
	ERR_FAIL_COND_V(p_mesh.is_null(), -1.0f);
	DisplacementSampler sampler;
	if (!sampler.setup(p_material) || p_edge_length <= 0.0f) {
		return -1.0f;
	}
	// The texels per meter on the surface: the UV area (in texels) over the area, over all the triangles.
	double texel_area = 0.0;
	double area = 0.0;
	for (int s = 0; s < p_mesh->get_surface_count(); s++) {
		if (p_mesh->surface_get_primitive_type(s) != Mesh::PRIMITIVE_TRIANGLES) {
			continue;
		}
		const Array arrays = p_mesh->surface_get_arrays(s);
		const PackedVector3Array v = arrays[Mesh::ARRAY_VERTEX];
		const PackedVector2Array uv = arrays[Mesh::ARRAY_TEX_UV];
		if (uv.size() != v.size()) {
			continue;
		}
		const PackedInt32Array indices = surface_indices(arrays);
		for (int i = 0; i + 2 < indices.size(); i += 3) {
			const int a = indices[i];
			const int b = indices[i + 1];
			const int c = indices[i + 2];
			area += 0.5 * (v[b] - v[a]).cross(v[c] - v[a]).length();
			const Vector2 ta = uv[a] * sampler.uv_scale * Vector2(sampler.width, sampler.height);
			const Vector2 tb = uv[b] * sampler.uv_scale * Vector2(sampler.width, sampler.height);
			const Vector2 tc = uv[c] * sampler.uv_scale * Vector2(sampler.width, sampler.height);
			texel_area += 0.5 * Math::abs((tb - ta).cross(tc - ta));
		}
	}
	if (area <= 0.0 || texel_area <= 0.0) {
		return -1.0f;
	}
	const double texels_per_edge = Math::sqrt(texel_area / area) * p_edge_length;
	return MAX(0.0f, float(Math::log2(texels_per_edge)));
}

Ref<ArrayMesh> MeshTessellator::tessellate(const Ref<Mesh> &p_mesh, const Ref<Material> &p_material_override) const {
	ERR_FAIL_COND_V(p_mesh.is_null(), Ref<ArrayMesh>());
	ERR_FAIL_COND_V_MSG(target_edge_length <= 0.0f, Ref<ArrayMesh>(), "MeshTessellator: The target edge length must be positive.");

	Ref<ArrayMesh> result;
	result.instantiate();
	Ref<ArrayMesh> source_array_mesh = p_mesh;
	for (int i = 0; i < p_mesh->get_blend_shape_count(); i++) {
		result->add_blend_shape(p_mesh->get_blend_shape_name(i));
	}
	if (source_array_mesh.is_valid()) {
		result->set_blend_shape_mode(source_array_mesh->get_blend_shape_mode());
	}

	float reach = 0.0f;
	bool displaced = false;
	int64_t budget = max_triangles;

	for (int s = 0; s < p_mesh->get_surface_count(); s++) {
		const Array arrays = p_mesh->surface_get_arrays(s);
		const TypedArray<Array> blend_shapes = p_mesh->surface_get_blend_shape_arrays(s);
		const Mesh::PrimitiveType primitive = p_mesh->surface_get_primitive_type(s);
		const uint64_t format = p_mesh->surface_get_format(s);
		uint64_t flags = format & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS;
		for (int c = 0; c < RSE::ARRAY_CUSTOM_COUNT; c++) {
			flags |= format & ((uint64_t)Mesh::ARRAY_FORMAT_CUSTOM_MASK << (Mesh::ARRAY_FORMAT_CUSTOM_BASE + c * Mesh::ARRAY_FORMAT_CUSTOM_BITS));
		}
		const Ref<Material> surface_material = p_mesh->surface_get_material(s);
		const String surface_name = source_array_mesh.is_valid() ? source_array_mesh->surface_get_name(s) : String();

		if (primitive != Mesh::PRIMITIVE_TRIANGLES) {
			result->add_surface_from_arrays(primitive, arrays, blend_shapes, Dictionary(), flags);
			result->surface_set_material(result->get_surface_count() - 1, surface_material);
			result->surface_set_name(result->get_surface_count() - 1, surface_name);
			continue;
		}

		const PackedVector3Array positions = arrays[Mesh::ARRAY_VERTEX];
		const PackedVector3Array normals = arrays[Mesh::ARRAY_NORMAL];
		const PackedInt32Array indices = surface_indices(arrays);

		SurfaceTessellation tess(positions, normals, indices, phong_enabled, phong_shape);
		tess.run(target_edge_length, MAX<int64_t>(budget, indices.size() / 3));
		budget -= tess.triangles.size();
		if (tess.truncated) {
			WARN_PRINT(vformat("MeshTessellator: Surface %d reached the triangle limit (%d) before the target edge length.", s, max_triangles));
		}

		Array out;
		out.resize(Mesh::ARRAY_MAX);
		PackedVector3Array out_positions;
		out_positions.resize(tess.vertex_positions.size());
		for (uint32_t i = 0; i < tess.vertex_positions.size(); i++) {
			out_positions.write[i] = tess.vertex_positions[i];
		}
		out[Mesh::ARRAY_VERTEX] = out_positions;

		PackedVector3Array out_normals = interpolate_attribute(normals, tess, indices);
		for (int i = tess.original_vertex_count; i < out_normals.size(); i++) {
			out_normals.write[i] = out_normals[i].normalized();
		}
		if (!out_normals.is_empty()) {
			out[Mesh::ARRAY_NORMAL] = out_normals;
		}

		const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];
		PackedFloat32Array out_tangents = interpolate_packed(tangents, 4, true, tess, indices);
		if (!out_tangents.is_empty()) {
			const PackedFloat32Array nearest = interpolate_packed(tangents, 4, false, tess, indices);
			float *w = out_tangents.ptrw();
			for (int i = tess.original_vertex_count; i < out_tangents.size() / 4; i++) {
				const Vector3 t = Vector3(w[i * 4 + 0], w[i * 4 + 1], w[i * 4 + 2]).normalized();
				w[i * 4 + 0] = t.x;
				w[i * 4 + 1] = t.y;
				w[i * 4 + 2] = t.z;
				w[i * 4 + 3] = nearest[i * 4 + 3] < 0.0f ? -1.0f : 1.0f;
			}
			out[Mesh::ARRAY_TANGENT] = out_tangents;
		}

		const PackedColorArray colors = interpolate_attribute(PackedColorArray(arrays[Mesh::ARRAY_COLOR]), tess, indices);
		if (!colors.is_empty()) {
			out[Mesh::ARRAY_COLOR] = colors;
		}
		const PackedVector2Array uvs = interpolate_attribute(PackedVector2Array(arrays[Mesh::ARRAY_TEX_UV]), tess, indices);
		if (!uvs.is_empty()) {
			out[Mesh::ARRAY_TEX_UV] = uvs;
		}
		const PackedVector2Array uv2s = interpolate_attribute(PackedVector2Array(arrays[Mesh::ARRAY_TEX_UV2]), tess, indices);
		if (!uv2s.is_empty()) {
			out[Mesh::ARRAY_TEX_UV2] = uv2s;
		}

		for (int c = 0; c < RSE::ARRAY_CUSTOM_COUNT; c++) {
			const int custom_format = (format >> (Mesh::ARRAY_FORMAT_CUSTOM_BASE + c * Mesh::ARRAY_FORMAT_CUSTOM_BITS)) & Mesh::ARRAY_FORMAT_CUSTOM_MASK;
			const Variant custom = arrays[Mesh::ARRAY_CUSTOM0 + c];
			if (custom.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
				const int width = custom_format - Mesh::ARRAY_CUSTOM_R_FLOAT + 1;
				out[Mesh::ARRAY_CUSTOM0 + c] = interpolate_packed(PackedFloat32Array(custom), width, true, tess, indices);
			} else if (custom.get_type() == Variant::PACKED_BYTE_ARRAY) {
				const int width = custom_format == Mesh::ARRAY_CUSTOM_RGBA_HALF ? 8 : 4;
				out[Mesh::ARRAY_CUSTOM0 + c] = interpolate_packed(PackedByteArray(custom), width, false, tess, indices);
			}
		}

		const PackedInt32Array bones = arrays[Mesh::ARRAY_BONES];
		const PackedFloat32Array weights = arrays[Mesh::ARRAY_WEIGHTS];
		const int influences = (format & Mesh::ARRAY_FLAG_USE_8_BONE_WEIGHTS) ? 8 : 4;
		if (bones.size() == positions.size() * influences && weights.size() == bones.size()) {
			// The corners' influences weighed by the place, the strongest kept.
			const uint32_t total = tess.vertex_positions.size();
			PackedInt32Array out_bones = bones;
			PackedFloat32Array out_weights = weights;
			out_bones.resize(total * influences);
			out_weights.resize(total * influences);
			for (uint32_t i = tess.original_vertex_count; i < total; i++) {
				const uint32_t j = i - tess.original_vertex_count;
				const uint32_t o = tess.vertex_original[j];
				LocalVector<Pair<float, int>> sum;
				for (int k = 0; k < 3; k++) {
					const int corner = indices[o * 3 + k];
					for (int n = 0; n < influences; n++) {
						const float wgt = weights[corner * influences + n] * tess.vertex_bary[j][k];
						if (wgt <= 0.0f) {
							continue;
						}
						const int bone = bones[corner * influences + n];
						bool found = false;
						for (Pair<float, int> &e : sum) {
							if (e.second == bone) {
								e.first += wgt;
								found = true;
								break;
							}
						}
						if (!found) {
							sum.push_back(Pair<float, int>(wgt, bone));
						}
					}
				}
				sum.sort_custom<PairSort<float, int>>();
				float total_weight = 0.0f;
				for (int n = 0; n < influences && n < (int)sum.size(); n++) {
					total_weight += sum[sum.size() - 1 - n].first;
				}
				for (int n = 0; n < influences; n++) {
					const bool used = n < (int)sum.size() && total_weight > 0.0f;
					out_bones.write[i * influences + n] = used ? sum[sum.size() - 1 - n].second : 0;
					out_weights.write[i * influences + n] = used ? sum[sum.size() - 1 - n].first / total_weight : 0.0f;
				}
			}
			out[Mesh::ARRAY_BONES] = out_bones;
			out[Mesh::ARRAY_WEIGHTS] = out_weights;
		}

		PackedInt32Array out_indices;
		out_indices.resize(tess.triangles.size() * 3);
		for (uint32_t t = 0; t < tess.triangles.size(); t++) {
			for (int k = 0; k < 3; k++) {
				out_indices.write[t * 3 + k] = tess.triangles[t].v[k];
			}
		}
		out[Mesh::ARRAY_INDEX] = out_indices;

		// The blend shapes move the new vertices as their corners move them.
		TypedArray<Array> out_blend_shapes;
		for (int b = 0; b < blend_shapes.size(); b++) {
			const Array shape = blend_shapes[b];
			Array out_shape;
			out_shape.resize(Mesh::ARRAY_MAX);
			const PackedVector3Array shape_positions = shape[Mesh::ARRAY_VERTEX];
			if (shape_positions.size() == positions.size()) {
				PackedVector3Array deltas;
				deltas.resize(positions.size());
				for (int i = 0; i < positions.size(); i++) {
					deltas.write[i] = shape_positions[i] - positions[i];
				}
				deltas = interpolate_attribute(deltas, tess, indices);
				for (int i = 0; i < deltas.size(); i++) {
					deltas.write[i] += out_positions[i];
				}
				out_shape[Mesh::ARRAY_VERTEX] = deltas;
			}
			PackedVector3Array shape_normals = interpolate_attribute(PackedVector3Array(shape[Mesh::ARRAY_NORMAL]), tess, indices);
			for (int i = tess.original_vertex_count; i < shape_normals.size(); i++) {
				shape_normals.write[i] = shape_normals[i].normalized();
			}
			if (!shape_normals.is_empty()) {
				out_shape[Mesh::ARRAY_NORMAL] = shape_normals;
			}
			const PackedFloat32Array shape_tangents = interpolate_packed(PackedFloat32Array(shape[Mesh::ARRAY_TANGENT]), 4, true, tess, indices);
			if (!shape_tangents.is_empty()) {
				out_shape[Mesh::ARRAY_TANGENT] = shape_tangents;
			}
			out_blend_shapes.push_back(out_shape);
		}

		// The LODs are simplified on the shape the material gives the surface: the indices only, the vertices stay.
		const Ref<Material> material = p_material_override.is_valid() ? p_material_override : surface_material;
		DisplacementSampler sampler;
		const bool surface_displaced = sampler.setup(material) && !out_normals.is_empty() && !uvs.is_empty();
		if (surface_displaced) {
			displaced = true;
			reach = MAX(reach, sampler.reach());
		}
		Dictionary lods;
		if (generate_lods) {
			PackedVector3Array shape_positions = out_positions;
			if (surface_displaced) {
				shape_positions = get_displaced_vertices(out, material);
			}
			// The normals of the shape, for the simplifier's attribute error.
			PackedVector3Array shape_normals;
			shape_normals.resize(shape_positions.size());
			Vector3 *nw = shape_normals.ptrw();
			for (int i = 0; i < out_indices.size(); i += 3) {
				const Vector3 &p0 = shape_positions[out_indices[i]];
				const Vector3 n = (shape_positions[out_indices[i + 2]] - p0).cross(shape_positions[out_indices[i + 1]] - p0); // Godot's clockwise front.
				for (int k = 0; k < 3; k++) {
					nw[out_indices[i + k]] += n;
				}
			}
			for (int i = 0; i < shape_normals.size(); i++) {
				nw[i] = nw[i].normalized();
			}
			Array shape;
			shape.resize(Mesh::ARRAY_MAX);
			shape[Mesh::ARRAY_VERTEX] = shape_positions;
			shape[Mesh::ARRAY_NORMAL] = shape_normals;
			if (!uvs.is_empty()) {
				shape[Mesh::ARRAY_TEX_UV] = uvs;
			}
			shape[Mesh::ARRAY_INDEX] = out_indices;
			Ref<ImporterMesh> simplifier;
			simplifier.instantiate();
			simplifier->add_surface(Mesh::PRIMITIVE_TRIANGLES, shape);
			simplifier->generate_lods(60.0f, Array());
			for (int l = 0; l < simplifier->get_surface_lod_count(0); l++) {
				lods[simplifier->get_surface_lod_size(0, l)] = simplifier->get_surface_lod_indices(0, l);
			}
		}

		result->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, out, out_blend_shapes, lods, flags);
		result->surface_set_material(result->get_surface_count() - 1, surface_material);
		result->surface_set_name(result->get_surface_count() - 1, surface_name);
	}

	if (displaced) {
		// The vertex shader moves the surface out of the mesh's box: the culling box takes the displacement's reach.
		result->set_custom_aabb(result->get_aabb().grow(reach));
	}
	return result;
}

void MeshTessellator::set_target_edge_length(float p_length) {
	target_edge_length = MAX(p_length, 0.0001f);
}

float MeshTessellator::get_target_edge_length() const {
	return target_edge_length;
}

void MeshTessellator::set_phong_enabled(bool p_enable) {
	phong_enabled = p_enable;
}

bool MeshTessellator::is_phong_enabled() const {
	return phong_enabled;
}

void MeshTessellator::set_phong_shape(float p_shape) {
	phong_shape = CLAMP(p_shape, 0.0f, 1.0f);
}

float MeshTessellator::get_phong_shape() const {
	return phong_shape;
}

void MeshTessellator::set_generate_lods(bool p_enable) {
	generate_lods = p_enable;
}

bool MeshTessellator::is_generating_lods() const {
	return generate_lods;
}

void MeshTessellator::set_max_triangles(int p_count) {
	max_triangles = MAX(p_count, 1);
}

int MeshTessellator::get_max_triangles() const {
	return max_triangles;
}

void MeshTessellator::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_target_edge_length", "length"), &MeshTessellator::set_target_edge_length);
	ClassDB::bind_method(D_METHOD("get_target_edge_length"), &MeshTessellator::get_target_edge_length);
	ClassDB::bind_method(D_METHOD("set_phong_enabled", "enable"), &MeshTessellator::set_phong_enabled);
	ClassDB::bind_method(D_METHOD("is_phong_enabled"), &MeshTessellator::is_phong_enabled);
	ClassDB::bind_method(D_METHOD("set_phong_shape", "shape"), &MeshTessellator::set_phong_shape);
	ClassDB::bind_method(D_METHOD("get_phong_shape"), &MeshTessellator::get_phong_shape);
	ClassDB::bind_method(D_METHOD("set_generate_lods", "enable"), &MeshTessellator::set_generate_lods);
	ClassDB::bind_method(D_METHOD("is_generating_lods"), &MeshTessellator::is_generating_lods);
	ClassDB::bind_method(D_METHOD("set_max_triangles", "count"), &MeshTessellator::set_max_triangles);
	ClassDB::bind_method(D_METHOD("get_max_triangles"), &MeshTessellator::get_max_triangles);
	ClassDB::bind_method(D_METHOD("tessellate", "mesh", "material_override"), &MeshTessellator::tessellate, DEFVAL(Ref<Material>()));
	ClassDB::bind_static_method("MeshTessellator", D_METHOD("get_displaced_vertices", "arrays", "material"), &MeshTessellator::get_displaced_vertices);
	ClassDB::bind_static_method("MeshTessellator", D_METHOD("get_displacement_texture_lod", "mesh", "material", "edge_length"), &MeshTessellator::get_displacement_texture_lod); // Mariner G7c.

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "target_edge_length", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:m"), "set_target_edge_length", "get_target_edge_length");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "phong_enabled"), "set_phong_enabled", "is_phong_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "phong_shape", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_phong_shape", "get_phong_shape");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "generate_lods"), "set_generate_lods", "is_generating_lods");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_triangles", PROPERTY_HINT_RANGE, "1,100000000,1,or_greater"), "set_max_triangles", "get_max_triangles");
}
