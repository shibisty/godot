/**************************************************************************/
/*  octahedral_impostor_baker.cpp                                         */
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

#include "octahedral_impostor_baker.h"

#include "core/object/class_db.h"
#include "scene/3d/box_impostor_baker.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

void OctahedralImpostorBaker::set_frames(int p_frames) {
	frames = CLAMP(p_frames, 2, 32);
}

int OctahedralImpostorBaker::get_frames() const {
	return frames;
}

void OctahedralImpostorBaker::set_frame_size(int p_size) {
	frame_size = CLAMP(p_size, 16, 1024);
}

int OctahedralImpostorBaker::get_frame_size() const {
	return frame_size;
}

void OctahedralImpostorBaker::set_hemisphere(bool p_enable) {
	hemisphere = p_enable;
}

bool OctahedralImpostorBaker::is_hemisphere() const {
	return hemisphere;
}

void OctahedralImpostorBaker::set_padding(int p_padding) {
	padding = CLAMP(p_padding, 0, 32);
}

int OctahedralImpostorBaker::get_padding() const {
	return padding;
}

namespace {

float sign_not_zero(float p_v) {
	return p_v < 0.0f ? -1.0f : 1.0f;
}

// A unit vector on the octahedron, -1 .. 1 (the normals in the atlas).
Vector2 encode_octahedral_normal(const Vector3 &p_n) {
	const Vector3 n = p_n / MAX(Math::abs(p_n.x) + Math::abs(p_n.y) + Math::abs(p_n.z), 1e-6f);
	if (n.z >= 0.0f) {
		return Vector2(n.x, n.y);
	}
	return Vector2((1.0f - Math::abs(n.y)) * sign_not_zero(n.x), (1.0f - Math::abs(n.x)) * sign_not_zero(n.y));
}

} // namespace

Vector3 OctahedralImpostorBaker::frame_direction(const Vector2 &p_grid, int p_frames, bool p_hemisphere) {
	const Vector2 e = p_grid / float(MAX(p_frames - 1, 1)) * 2.0f - Vector2(1, 1);
	Vector3 d;
	if (p_hemisphere) {
		const Vector2 h = Vector2(e.x + e.y, e.x - e.y) * 0.5f;
		d = Vector3(h.x, 1.0f - Math::abs(h.x) - Math::abs(h.y), h.y);
	} else {
		d = Vector3(e.x, 1.0f - Math::abs(e.x) - Math::abs(e.y), e.y);
		if (d.y < 0.0f) {
			const float x = d.x;
			d.x = (1.0f - Math::abs(d.z)) * sign_not_zero(x);
			d.z = (1.0f - Math::abs(x)) * sign_not_zero(d.z);
		}
	}
	return d.normalized();
}

Vector2 OctahedralImpostorBaker::direction_to_grid(const Vector3 &p_direction, int p_frames, bool p_hemisphere) {
	Vector3 d = p_direction;
	if (p_hemisphere) {
		d.y = MAX(d.y, 0.0f);
	}
	d /= MAX(Math::abs(d.x) + Math::abs(d.y) + Math::abs(d.z), 1e-6f);
	Vector2 e;
	if (p_hemisphere) {
		e = Vector2(d.x + d.z, d.x - d.z);
	} else {
		e = Vector2(d.x, d.z);
		if (d.y < 0.0f) {
			e = Vector2((1.0f - Math::abs(d.z)) * sign_not_zero(d.x), (1.0f - Math::abs(d.x)) * sign_not_zero(d.z));
		}
	}
	return (e * 0.5f + Vector2(0.5f, 0.5f)) * float(MAX(p_frames - 1, 1));
}

Basis OctahedralImpostorBaker::frame_basis(const Vector3 &p_direction) {
	const Vector3 up_ref = Math::abs(p_direction.y) > 0.999f ? Vector3(0, 0, 1) : Vector3(0, 1, 0);
	const Vector3 r = up_ref.cross(p_direction).normalized();
	const Vector3 u = p_direction.cross(r);
	return Basis(r, u, p_direction); // Columns.
}

namespace {

void gather_octahedral_instances(Node *p_node, LocalVector<VisualInstance3D *> &r_list) {
	VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node);
	if (vi && vi->is_visible_in_tree()) {
		r_list.push_back(vi);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		gather_octahedral_instances(p_node->get_child(i), r_list);
	}
}

void gather_octahedral_geometry(Node *p_node, LocalVector<GeometryInstance3D *> &r_list) {
	GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(p_node);
	if (gi) {
		r_list.push_back(gi);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		gather_octahedral_geometry(p_node->get_child(i), r_list);
	}
}

const char *octahedral_impostor_shader_code = R"(// Mariner G5e: an octahedral impostor (OctahedralImpostorBaker).
shader_type spatial;
render_mode cull_disabled, depth_draw_opaque;

uniform sampler2D impostor_albedo : source_color, filter_linear_mipmap_anisotropic, repeat_disable;
// RG: the object-space normal (octahedral), B: the depth of the model's front, A: where the ray leaves the part it
// entered there (0: the front of the bounding sphere, 1: its back).
uniform sampler2D impostor_normal_depth : hint_default_white, filter_nearest_mipmap, repeat_disable;
uniform int impostor_frames = 12;
uniform bool impostor_hemisphere = false;
uniform vec3 impostor_center;
uniform float impostor_radius = 1.0;
uniform float impostor_alpha_cut : hint_range(0.0, 1.0) = 0.35;
uniform float roughness : hint_range(0.0, 1.0) = 0.85;
uniform int impostor_steps : hint_range(1, 256) = 64;

varying flat vec2 imp_grid0;
varying flat vec2 imp_grid1;
varying flat vec2 imp_grid2;
varying flat vec3 imp_weights;
varying vec3 imp_obj; // The quad's point in object space.
varying vec3 imp_ray; // From the eye through the point (orthographic: along the view), object space.

float imp_sign(float v) {
	return v < 0.0 ? -1.0 : 1.0;
}

vec3 imp_direction(vec2 grid) {
	vec2 e = grid / float(max(impostor_frames - 1, 1)) * 2.0 - 1.0;
	vec3 d;
	if (impostor_hemisphere) {
		vec2 h = vec2(e.x + e.y, e.x - e.y) * 0.5;
		d = vec3(h.x, 1.0 - abs(h.x) - abs(h.y), h.y);
	} else {
		d = vec3(e.x, 1.0 - abs(e.x) - abs(e.y), e.y);
		if (d.y < 0.0) {
			float x = d.x;
			d.x = (1.0 - abs(d.z)) * imp_sign(x);
			d.z = (1.0 - abs(x)) * imp_sign(d.z);
		}
	}
	return normalize(d);
}

vec2 imp_to_grid(vec3 d) {
	if (impostor_hemisphere) {
		d.y = max(d.y, 0.0);
	}
	d /= max(abs(d.x) + abs(d.y) + abs(d.z), 1e-6);
	vec2 e;
	if (impostor_hemisphere) {
		e = vec2(d.x + d.z, d.x - d.z);
	} else {
		e = d.xz;
		if (d.y < 0.0) {
			e = vec2((1.0 - abs(d.z)) * imp_sign(d.x), (1.0 - abs(d.x)) * imp_sign(d.z));
		}
	}
	return (e * 0.5 + 0.5) * float(max(impostor_frames - 1, 1));
}

vec3 imp_decode_normal(vec2 e) {
	e = e * 2.0 - 1.0;
	vec3 n = vec3(e, 1.0 - abs(e.x) - abs(e.y));
	if (n.z < 0.0) {
		n.xy = (1.0 - abs(n.yx)) * vec2(imp_sign(n.x), imp_sign(n.y));
	}
	return normalize(n);
}

mat3 imp_basis(vec3 d) {
	vec3 up_ref = abs(d.y) > 0.999 ? vec3(0.0, 0.0, 1.0) : vec3(0.0, 1.0, 0.0);
	vec3 r = normalize(cross(up_ref, d));
	return mat3(r, cross(d, r), d);
}

void vertex() {
	mat4 inv_model = inverse(MODEL_MATRIX);
	bool ortho = PROJECTION_MATRIX[3][3] == 1.0;
	vec3 eye = (inv_model * vec4(CAMERA_POSITION_WORLD, 1.0)).xyz;
	vec3 axis = (inv_model * vec4(-INV_VIEW_MATRIX[2].xyz, 0.0)).xyz;
	vec3 to_eye = ortho ? -axis : eye - impostor_center;
	float dist = length(to_eye);
	vec3 v = to_eye / max(dist, 1e-6);
	// The 3 frames around the view direction and their weights.
	vec2 g = clamp(imp_to_grid(v), vec2(0.0), vec2(float(impostor_frames - 1)));
	vec2 cell = min(floor(g), vec2(float(impostor_frames - 2)));
	vec2 f = g - cell;
	if (f.x + f.y < 1.0) {
		imp_grid0 = cell;
		imp_weights = vec3(1.0 - f.x - f.y, f.x, f.y);
	} else {
		imp_grid0 = cell + vec2(1.0);
		imp_weights = vec3(f.x + f.y - 1.0, 1.0 - f.y, 1.0 - f.x);
	}
	imp_grid1 = cell + vec2(1.0, 0.0);
	imp_grid2 = cell + vec2(0.0, 1.0);
	// A quad through the center facing the eye, as large as the bounding sphere seen from there.
	mat3 b = imp_basis(v);
	float half_size = impostor_radius;
	if (!ortho) {
		half_size *= dist / sqrt(max(dist * dist - impostor_radius * impostor_radius, 0.01 * impostor_radius * impostor_radius));
	}
	half_size = min(half_size, impostor_radius * 4.0);
	vec2 corner = UV * 2.0 - 1.0;
	VERTEX = impostor_center + (b[0] * corner.x - b[1] * corner.y) * half_size;
	NORMAL = v;
	TANGENT = b[0];
	BINORMAL = b[1];
	imp_obj = VERTEX;
	imp_ray = ortho ? axis : VERTEX - eye;
}

// The hit of the ray with one frame's relief: the frame-space point (xyz), the atlas UV, and whether the ray met the
// model at all (a ray that passes by would read whatever lies at its exit).
vec3 imp_trace(vec2 grid, vec3 o, vec3 dir, out vec2 r_uv, out vec2 r_ddx, out vec2 r_ddy, out mat3 r_basis, out bool r_hit) {
	vec3 d = imp_direction(grid);
	mat3 b = imp_basis(d);
	r_basis = b;
	vec3 q = o * b; // The point in the frame's space (transpose(b) * o).
	vec3 v = dir * b;
	float inv_size = 0.5 / impostor_radius;
	float n = float(impostor_frames);
	vec2 tile = grid / n;
	r_ddx = vec2(dFdx(q.x), -dFdx(q.y)) * inv_size / n;
	r_ddy = vec2(dFdy(q.x), -dFdy(q.y)) * inv_size / n;
	vec2 tex_size = vec2(textureSize(impostor_normal_depth, 0));
	vec2 lo = tile + 0.5 / tex_size;
	vec2 hi = tile + 1.0 / n - 0.5 / tex_size;
	// From the front of the sphere (z = R) to its back (z = -R) along the ray (v.z < 0 toward the model); the model is
	// solid between its front and back depths.
	float vz = min(v.z, -1e-4);
	float t0 = (impostor_radius - q.z) / vz;
	float t1 = (-impostor_radius - q.z) / vz;
	float hit_t = t1;
	float prev_t = t0;
	r_hit = false;
	for (int i = 0; i <= impostor_steps; i++) {
		float t = mix(t0, t1, float(i) / float(impostor_steps));
		vec3 p = q + v * t;
		vec2 uv = clamp(tile + vec2(p.x * inv_size + 0.5, 0.5 - p.y * inv_size) / n, lo, hi);
		vec2 fb = textureGrad(impostor_normal_depth, uv, r_ddx, r_ddy).ba;
		float h = (impostor_radius - p.z) * inv_size;
		if (h >= fb.x && h <= fb.y) {
			// Inside: the entry between the last step outside and this one, by bisection.
			float a = prev_t;
			float b = t;
			for (int k = 0; k < 6 && i > 0; k++) {
				float m = 0.5 * (a + b);
				vec3 pm = q + v * m;
				vec2 uvm = clamp(tile + vec2(pm.x * inv_size + 0.5, 0.5 - pm.y * inv_size) / n, lo, hi);
				vec2 fbm = textureGrad(impostor_normal_depth, uvm, r_ddx, r_ddy).ba;
				float hm = (impostor_radius - pm.z) * inv_size;
				if (hm >= fbm.x && hm <= fbm.y) {
					b = m;
				} else {
					a = m;
				}
			}
			hit_t = b;
			r_hit = true;
			break;
		}
		prev_t = t;
	}
	vec3 p = q + v * hit_t;
	r_uv = clamp(tile + vec2(p.x * inv_size + 0.5, 0.5 - p.y * inv_size) / n, lo, hi);
	return p;
}

void fragment() {
	vec3 albedo = vec3(0.0);
	vec3 normal = vec3(0.0);
	vec3 hit = vec3(0.0);
	float alpha = 0.0;
	float weight = 0.0;
	for (int k = 0; k < 3; k++) {
		vec2 grid = k == 0 ? imp_grid0 : (k == 1 ? imp_grid1 : imp_grid2);
		float w = imp_weights[k];
		if (w <= 1e-4) {
			continue;
		}
		vec2 uv, ddx, ddy;
		mat3 b;
		bool met;
		vec3 p = imp_trace(grid, imp_obj - impostor_center, imp_ray, uv, ddx, ddy, b, met);
		weight += w;
		if (!met) {
			continue;
		}
		vec4 c = textureGrad(impostor_albedo, uv, ddx, ddy);
		vec4 nd = textureGrad(impostor_normal_depth, uv, ddx, ddy);
		float wa = w * c.a;
		albedo += c.rgb * wa;
		normal += imp_decode_normal(nd.rg) * wa;
		hit += (impostor_center + b * p) * wa;
		alpha += wa;
	}
	alpha /= max(weight, 1e-6);
	if (alpha < impostor_alpha_cut) {
		discard;
	}
	float inv = 1.0 / max(alpha * weight, 1e-6);
	ALBEDO = albedo * inv;
	ROUGHNESS = roughness;
	vec3 n_obj = normal * inv;
	NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(n_obj, 0.0))).xyz);
	vec4 clip = PROJECTION_MATRIX * (VIEW_MATRIX * (MODEL_MATRIX * vec4(hit * inv, 1.0)));
	DEPTH = clip.z / clip.w;
#if CURRENT_RENDERER == RENDERER_COMPATIBILITY
	DEPTH = DEPTH * 0.5 + 0.5; // OpenGL's clip space depth is -1 .. 1.
#endif
}
)";

} // namespace

Ref<Shader> OctahedralImpostorBaker::get_shader() {
	Ref<Shader> shader;
	shader.instantiate();
	shader->set_code(octahedral_impostor_shader_code);
	return shader;
}

Ref<ArrayMesh> OctahedralImpostorBaker::bake(Node3D *p_source) {
	ERR_FAIL_NULL_V(p_source, Ref<ArrayMesh>());
	ERR_FAIL_COND_V_MSG(!p_source->is_inside_tree(), Ref<ArrayMesh>(), "The source of an octahedral impostor must be inside the scene tree.");
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL_V(tree, Ref<ArrayMesh>());

	// The bounding sphere in the source's space.
	const Transform3D to_source = p_source->get_global_transform().affine_inverse();
	LocalVector<VisualInstance3D *> instances;
	gather_octahedral_instances(p_source, instances);
	AABB bounds;
	bool any = false;
	for (VisualInstance3D *vi : instances) {
		const AABB aabb = (to_source * vi->get_global_transform()).xform(vi->get_aabb());
		if (!any) {
			bounds = aabb;
			any = true;
		} else {
			bounds.merge_with(aabb);
		}
	}
	ERR_FAIL_COND_V_MSG(!any, Ref<ArrayMesh>(), "An octahedral impostor's source has no visible geometry.");
	center = bounds.get_center();
	radius = MAX(bounds.size.length() * 0.5f, 0.01f) * 1.01f;

	// The scene to photograph: a copy of the source (no scripts) in its own world.
	const float margin = 0.05f * radius;
	SubViewport *vp = memnew(SubViewport);
	vp->set_use_own_world_3d(true);
	vp->set_transparent_background(true);
	vp->set_use_hdr_2d(true); // Linear float output: exact colors and depths.
	vp->set_update_mode(SubViewport::UPDATE_ALWAYS);
	vp->set_msaa_3d(Viewport::MSAA_DISABLED);
	vp->set_screen_space_aa(Viewport::SCREEN_SPACE_AA_DISABLED);
	vp->set_use_taa(false);
	vp->set_use_debanding(false);
	vp->set_mesh_lod_threshold(0.0);
	vp->set_scaling_3d_scale(1.0);
	vp->set_size(Size2i(frame_size, frame_size));
	Camera3D *cam = memnew(Camera3D);
	Ref<Environment> env;
	env.instantiate();
	env->set_background(Environment::BG_CLEAR_COLOR);
	env->set_tonemapper(Environment::TONE_MAPPER_LINEAR);
	cam->set_environment(env);
	cam->set_orthogonal(2.0f * radius, 0.0f, 2.0f * (radius + margin));
	Node3D *copy = Object::cast_to<Node3D>(p_source->duplicate(Node::DUPLICATE_GROUPS));
	ERR_FAIL_NULL_V(copy, Ref<ArrayMesh>());
	copy->set_transform(Transform3D());
	vp->add_child(copy);
	vp->add_child(cam);
	tree->get_root()->add_child(vp);
	cam->make_current();
	LocalVector<GeometryInstance3D *> geometry;
	gather_octahedral_geometry(copy, geometry);

	const int count = frames * frames;
	LocalVector<Basis> bases;
	bases.resize(count);
	for (int y = 0; y < frames; y++) {
		for (int x = 0; x < frames; x++) {
			bases[y * frames + x] = frame_basis(frame_direction(Vector2(x, y), frames, hemisphere));
		}
	}
	auto photograph = [&](int p_frame) -> Ref<Image> {
		Transform3D xf;
		xf.basis = bases[p_frame];
		xf.origin = center + xf.basis.get_column(2) * (radius + margin);
		cam->set_transform(xf);
		tree->flush_transform_notifications();
		RS::get_singleton()->draw(false);
		RS::get_singleton()->draw(false);
		Ref<Image> img = vp->get_texture()->get_image();
		if (img.is_valid()) {
			img->convert(Image::FORMAT_RGBAF);
		}
		return img;
	};

	// 1. Albedo, unshaded (as BoxImpostorBaker does).
	for (GeometryInstance3D *gi : geometry) {
		auto unshaded = [](const Ref<Material> &p_material) -> Ref<Material> {
			Ref<BaseMaterial3D> bm = p_material;
			if (bm.is_null()) {
				return p_material;
			}
			Ref<BaseMaterial3D> dup = bm->duplicate();
			dup->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
			return dup;
		};
		if (gi->get_material_override().is_valid()) {
			gi->set_material_override(unshaded(gi->get_material_override()));
			continue;
		}
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(gi);
		if (mi && mi->get_mesh().is_valid()) {
			for (int s = 0; s < mi->get_mesh()->get_surface_count(); s++) {
				mi->set_surface_override_material(s, unshaded(mi->get_active_material(s)));
			}
		}
	}
	vp->set_debug_draw(Viewport::DEBUG_DRAW_UNSHADED);
	LocalVector<Ref<Image>> colors;
	colors.resize(count);
	for (int i = 0; i < count; i++) {
		colors[i] = photograph(i);
	}
	// 2. Normal (view space = the frame's space, RG) and depth from the front of the sphere (B), every surface opaque.
	Ref<Shader> nd_shader;
	nd_shader.instantiate();
	nd_shader->set_code(R"(
shader_type spatial;
render_mode unshaded, cull_disabled, fog_disabled;
uniform float bake_near;
uniform float bake_depth;
void fragment() {
	vec3 n = normalize(FRONT_FACING ? NORMAL : -NORMAL);
	ALBEDO = vec3(n.xy * 0.5 + 0.5, clamp((-VERTEX.z - bake_near) / bake_depth, 0.0, 1.0));
}
)");
	Ref<ShaderMaterial> nd_material;
	nd_material.instantiate();
	nd_material->set_shader(nd_shader);
	nd_material->set_shader_parameter("bake_near", margin);
	nd_material->set_shader_parameter("bake_depth", 2.0f * radius);
	vp->set_debug_draw(Viewport::DEBUG_DRAW_DISABLED);
	for (GeometryInstance3D *gi : geometry) {
		gi->set_material_override(nd_material);
	}
	LocalVector<Ref<Image>> normal_depths;
	normal_depths.resize(count);
	for (int i = 0; i < count; i++) {
		normal_depths[i] = photograph(i);
	}
	// 3. The depth of the nearest back faces (B): where the ray leaves the first (closed) part it meets - its thickness,
	// and the space behind it stays empty (the farthest surface would fill the gap between a jib and the quay below).
	Ref<Shader> back_shader;
	back_shader.instantiate();
	back_shader->set_code(R"(
shader_type spatial;
render_mode unshaded, cull_front, fog_disabled;
uniform float bake_near;
uniform float bake_depth;
void fragment() {
	ALBEDO = vec3(0.0, 0.0, clamp((-VERTEX.z - bake_near) / bake_depth, 0.0, 1.0));
}
)");
	Ref<ShaderMaterial> back_material;
	back_material.instantiate();
	back_material->set_shader(back_shader);
	back_material->set_shader_parameter("bake_near", margin);
	back_material->set_shader_parameter("bake_depth", 2.0f * radius);
	for (GeometryInstance3D *gi : geometry) {
		gi->set_material_override(back_material);
	}
	LocalVector<Ref<Image>> back_depths;
	back_depths.resize(count);
	for (int i = 0; i < count; i++) {
		back_depths[i] = photograph(i);
	}
	tree->get_root()->remove_child(vp);
	memdelete(vp);

	// The atlas: frames x frames tiles, the frame at grid point (x, y) in column x, row y.
	const int side = frames * frame_size;
	albedo_image = Image::create_empty(side, side, false, Image::FORMAT_RGBA8);
	normal_depth_image = Image::create_empty(side, side, false, Image::FORMAT_RGBA8);
	normal_depth_image->fill(Color(0.5, 0.5, 1.0, 1.0)); // Nothing there: at the back of the sphere.
	const float min_thickness = MAX(2.0f / float(frame_size), 3.0f / 255.0f);
	Vector<uint8_t> covered;
	covered.resize(side * side);
	covered.fill(0);
	for (int i = 0; i < count; i++) {
		ERR_CONTINUE(colors[i].is_null() || normal_depths[i].is_null() || back_depths[i].is_null());
		ERR_CONTINUE(colors[i]->get_width() != frame_size || normal_depths[i]->get_width() != frame_size);
		const Basis &b = bases[i];
		const Vector3 r = b.get_column(0);
		const Vector3 u = b.get_column(1);
		const Vector3 d = b.get_column(2);
		const int ox = (i % frames) * frame_size;
		const int oy = (i / frames) * frame_size;
		for (int y = 0; y < frame_size; y++) {
			for (int x = 0; x < frame_size; x++) {
				const Color nd = normal_depths[i]->get_pixel(x, y);
				if (nd.a < 0.5f) {
					continue;
				}
				covered.write[(oy + y) * side + ox + x] = 1;
				Color c = colors[i]->get_pixel(x, y);
				c = Color(CLAMP(c.r, 0.0f, 1.0f), CLAMP(c.g, 0.0f, 1.0f), CLAMP(c.b, 0.0f, 1.0f)).linear_to_srgb();
				c.a = 1.0;
				albedo_image->set_pixel(ox + x, oy + y, c);
				const Vector2 nv = Vector2(nd.r, nd.g) * 2.0f - Vector2(1, 1);
				const Vector3 n = (r * nv.x + u * nv.y + d * Math::sqrt(MAX(1.0f - nv.length_squared(), 0.0f))).normalized();
				const Vector2 e = encode_octahedral_normal(n) * 0.5f + Vector2(0.5f, 0.5f);
				const float front = CLAMP(nd.b, 0.0f, 1.0f);
				// At least 2 texels behind the front (the single-sided surfaces, the 8-bit depths).
				const Color bd = back_depths[i]->get_pixel(x, y);
				const float back = CLAMP(MAX(bd.a > 0.5f ? bd.b : front, front + min_thickness), 0.0f, 1.0f);
				normal_depth_image->set_pixel(ox + x, oy + y, Color(e.x, e.y, front, back));
			}
		}
	}
	// The empty texels near the model take its colors and normals (the depths stay at the back - dilate() keeps the
	// alpha, and the front depth (B) is put back): the filtering and the mipmaps do not pull the background into the
	// edges.
	BoxImpostorBaker::dilate_image(albedo_image, covered, padding + 2);
	BoxImpostorBaker::dilate_image(normal_depth_image, covered, padding + 2);
	for (int y = 0; y < side; y++) {
		for (int x = 0; x < side; x++) {
			if (!covered[y * side + x]) {
				Color c = normal_depth_image->get_pixel(x, y);
				c.b = 1.0;
				normal_depth_image->set_pixel(x, y, c);
			}
		}
	}
	albedo_image->generate_mipmaps();
	BoxImpostorBaker::preserve_alpha_coverage_mipmaps(albedo_image);
	normal_depth_image->generate_mipmaps();

	Ref<ShaderMaterial> material;
	material.instantiate();
	material->set_shader(get_shader());
	material->set_shader_parameter("impostor_albedo", ImageTexture::create_from_image(albedo_image));
	material->set_shader_parameter("impostor_normal_depth", ImageTexture::create_from_image(normal_depth_image));
	material->set_shader_parameter("impostor_frames", frames);
	material->set_shader_parameter("impostor_hemisphere", hemisphere);
	material->set_shader_parameter("impostor_center", center);
	material->set_shader_parameter("impostor_radius", radius);

	// One quad; the vertex shader turns it to the camera.
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array vertices = { center + Vector3(-radius, radius, 0), center + Vector3(radius, radius, 0), center + Vector3(radius, -radius, 0), center + Vector3(-radius, -radius, 0) };
	PackedVector3Array normals = { Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1), Vector3(0, 0, 1) };
	PackedVector2Array uvs = { Vector2(0, 0), Vector2(1, 0), Vector2(1, 1), Vector2(0, 1) };
	PackedInt32Array indices = { 0, 1, 2, 0, 2, 3 };
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays);
	mesh->surface_set_material(0, material);
	mesh->set_custom_aabb(AABB(center - Vector3(radius, radius, radius), Vector3(radius, radius, radius) * 2.0f));
	mesh->set_name(String(p_source->get_name()) + "OctahedralImpostor");
	return mesh;
}

void OctahedralImpostorBaker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_frames", "frames"), &OctahedralImpostorBaker::set_frames);
	ClassDB::bind_method(D_METHOD("get_frames"), &OctahedralImpostorBaker::get_frames);
	ClassDB::bind_method(D_METHOD("set_frame_size", "size"), &OctahedralImpostorBaker::set_frame_size);
	ClassDB::bind_method(D_METHOD("get_frame_size"), &OctahedralImpostorBaker::get_frame_size);
	ClassDB::bind_method(D_METHOD("set_hemisphere", "enable"), &OctahedralImpostorBaker::set_hemisphere);
	ClassDB::bind_method(D_METHOD("is_hemisphere"), &OctahedralImpostorBaker::is_hemisphere);
	ClassDB::bind_method(D_METHOD("set_padding", "padding"), &OctahedralImpostorBaker::set_padding);
	ClassDB::bind_method(D_METHOD("get_padding"), &OctahedralImpostorBaker::get_padding);
	ClassDB::bind_static_method("OctahedralImpostorBaker", D_METHOD("frame_direction", "grid", "frames", "hemisphere"), &OctahedralImpostorBaker::frame_direction);
	ClassDB::bind_static_method("OctahedralImpostorBaker", D_METHOD("direction_to_grid", "direction", "frames", "hemisphere"), &OctahedralImpostorBaker::direction_to_grid);
	ClassDB::bind_static_method("OctahedralImpostorBaker", D_METHOD("frame_basis", "direction"), &OctahedralImpostorBaker::frame_basis);
	ClassDB::bind_static_method("OctahedralImpostorBaker", D_METHOD("get_shader"), &OctahedralImpostorBaker::get_shader);
	ClassDB::bind_method(D_METHOD("bake", "source"), &OctahedralImpostorBaker::bake);
	ClassDB::bind_method(D_METHOD("get_albedo_image"), &OctahedralImpostorBaker::get_albedo_image);
	ClassDB::bind_method(D_METHOD("get_normal_depth_image"), &OctahedralImpostorBaker::get_normal_depth_image);
	ClassDB::bind_method(D_METHOD("get_center"), &OctahedralImpostorBaker::get_center);
	ClassDB::bind_method(D_METHOD("get_radius"), &OctahedralImpostorBaker::get_radius);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "frames", PROPERTY_HINT_RANGE, "2,32,1"), "set_frames", "get_frames");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "frame_size", PROPERTY_HINT_RANGE, "16,1024,1"), "set_frame_size", "get_frame_size");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hemisphere"), "set_hemisphere", "is_hemisphere");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "padding", PROPERTY_HINT_RANGE, "0,32,1"), "set_padding", "get_padding");
}
