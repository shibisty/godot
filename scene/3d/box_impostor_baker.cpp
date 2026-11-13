/**************************************************************************/
/*  box_impostor_baker.cpp                                                */
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

#include "box_impostor_baker.h"

#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/3d/visual_instance_3d.h"
#include "scene/main/scene_tree.h"
#include "scene/main/viewport.h"
#include "scene/main/window.h"
#include "scene/resources/environment.h"
#include "scene/resources/material.h"
#include "scene/resources/surface_tool.h"
#include "servers/rendering/rendering_server.h"

void BoxImpostorBaker::set_texels_per_meter(float p_texels) {
	texels_per_meter = MAX(p_texels, 0.01f);
}

float BoxImpostorBaker::get_texels_per_meter() const {
	return texels_per_meter;
}

void BoxImpostorBaker::set_max_size(int p_size) {
	max_size = CLAMP(p_size, 64, 16384);
}

int BoxImpostorBaker::get_max_size() const {
	return max_size;
}

void BoxImpostorBaker::set_padding(int p_padding) {
	padding = CLAMP(p_padding, 0, 64);
}

int BoxImpostorBaker::get_padding() const {
	return padding;
}

void BoxImpostorBaker::set_bake_bottom(bool p_enable) {
	bake_bottom = p_enable;
}

bool BoxImpostorBaker::is_bake_bottom_enabled() const {
	return bake_bottom;
}

void BoxImpostorBaker::set_hull(bool p_enable) {
	hull = p_enable;
}

bool BoxImpostorBaker::is_hull_enabled() const {
	return hull;
}

Vector<Point2i> BoxImpostorBaker::pack_square(const Vector<Size2i> &p_sizes, int p_padding, int &r_side) {
	const int count = p_sizes.size();
	Vector<Point2i> positions;
	positions.resize(count);
	if (count == 0) {
		r_side = 0;
		return positions;
	}
	// Tallest first, rows from the top.
	Vector<int> order;
	int64_t area = 0;
	for (int i = 0; i < count; i++) {
		order.push_back(i);
		area += int64_t(p_sizes[i].x + 2 * p_padding) * (p_sizes[i].y + 2 * p_padding);
	}
	for (int i = 1; i < count; i++) {
		for (int j = i; j > 0 && p_sizes[order[j]].y > p_sizes[order[j - 1]].y; j--) {
			SWAP(order.write[j], order.write[j - 1]);
		}
	}
	int side = MAX(int(Math::ceil(Math::sqrt(double(area)))), 1);
	for (int i = 0; i < count; i++) {
		side = MAX(side, p_sizes[i].x + 2 * p_padding);
	}
	while (true) {
		int x = 0;
		int y = 0;
		int row_h = 0;
		bool fits = true;
		for (int k = 0; k < count && fits; k++) {
			const Size2i s = p_sizes[order[k]] + Size2i(2 * p_padding, 2 * p_padding);
			if (x + s.x > side) {
				x = 0;
				y += row_h;
				row_h = 0;
			}
			if (y + s.y > side) {
				fits = false;
				break;
			}
			positions.write[order[k]] = Point2i(x + p_padding, y + p_padding);
			x += s.x;
			row_h = MAX(row_h, s.y);
		}
		if (fits) {
			r_side = side;
			return positions;
		}
		side += MAX(side / 32, 4);
	}
}

namespace {

struct ImpostorFace {
	Vector3 n; // Outward normal: the camera looks along -n.
	Vector3 r; // Right in the image (+U).
	Vector3 u; // Up in the image (-V).
	Vector3 center; // Of the face.
	Size2 cover; // The extent covered by the image (whole texels), meters.
	float depth = 0.0; // Of the box along n, meters.
	Size2i pixels;
	Point2i position; // In the atlas.
	Ref<Image> color; // Linear albedo; then the normal and depth pass; then the window pass.
	Ref<Image> normal_depth;
	Ref<Image> window;
};

void gather_instances(Node *p_node, LocalVector<VisualInstance3D *> &r_list) {
	VisualInstance3D *vi = Object::cast_to<VisualInstance3D>(p_node);
	if (vi && vi->is_visible_in_tree()) {
		r_list.push_back(vi);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		gather_instances(p_node->get_child(i), r_list);
	}
}

void gather_geometry(Node *p_node, LocalVector<GeometryInstance3D *> &r_list) {
	GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(p_node);
	if (gi) {
		r_list.push_back(gi);
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		gather_geometry(p_node->get_child(i), r_list);
	}
}

bool is_glass(const Ref<Material> &p_material) {
	Ref<BaseMaterial3D> bm = p_material;
	return bm.is_valid() && bm->get_transparency() != BaseMaterial3D::TRANSPARENCY_DISABLED && bm->get_transparency() != BaseMaterial3D::TRANSPARENCY_ALPHA_SCISSOR && bm->get_transparency() != BaseMaterial3D::TRANSPARENCY_ALPHA_HASH;
}

// Fills the empty texels of an image near covered ones with their neighbours' average (so that filtering and mipmaps
// do not pull the background into the edges).
void dilate(Ref<Image> &p_image, const Vector<uint8_t> &p_covered_in, int p_iterations) {
	const int w = p_image->get_width();
	const int h = p_image->get_height();
	Vector<uint8_t> covered = p_covered_in;
	for (int it = 0; it < p_iterations; it++) {
		Vector<uint8_t> next = covered;
		for (int y = 0; y < h; y++) {
			for (int x = 0; x < w; x++) {
				if (covered[y * w + x]) {
					continue;
				}
				Color sum;
				int n = 0;
				for (int dy = -1; dy <= 1; dy++) {
					for (int dx = -1; dx <= 1; dx++) {
						const int sx = x + dx;
						const int sy = y + dy;
						if (sx >= 0 && sy >= 0 && sx < w && sy < h && covered[sy * w + sx]) {
							sum += p_image->get_pixel(sx, sy);
							n++;
						}
					}
				}
				if (n > 0) {
					Color c = sum / float(n);
					c.a = p_image->get_pixel(x, y).a;
					p_image->set_pixel(x, y, c);
					next.write[y * w + x] = 1;
				}
			}
		}
		covered = next;
	}
}

// Mipmaps that keep the silhouette: the alpha of every level scaled so that as many texels pass the alpha scissor (0.5)
// as in the full-size image (coverage-preserving alpha), otherwise averaging with the empty texels around erodes it.
void preserve_alpha_coverage(Ref<Image> &p_image) {
	ERR_FAIL_COND(p_image->get_format() != Image::FORMAT_RGBA8);
	Vector<uint8_t> data = p_image->get_data();
	uint8_t *w = data.ptrw();
	auto coverage = [&](int64_t p_ofs, int p_count, float p_scale) {
		int n = 0;
		for (int i = 0; i < p_count; i++) {
			n += float(w[p_ofs + i * 4 + 3]) * p_scale >= 127.5f ? 1 : 0;
		}
		return float(n) / float(MAX(p_count, 1));
	};
	int64_t ofs0, size0;
	int w0, h0;
	p_image->get_mipmap_offset_size_and_dimensions(0, ofs0, size0, w0, h0);
	const float target = coverage(ofs0, w0 * h0, 1.0f);
	for (int m = 1; m <= p_image->get_mipmap_count(); m++) {
		int64_t ofs, size;
		int mw, mh;
		p_image->get_mipmap_offset_size_and_dimensions(m, ofs, size, mw, mh);
		float lo = 0.5f;
		float hi = 8.0f;
		for (int it = 0; it < 12; it++) {
			const float mid = (lo + hi) * 0.5f;
			if (coverage(ofs, mw * mh, mid) < target) {
				lo = mid;
			} else {
				hi = mid;
			}
		}
		for (int i = 0; i < mw * mh; i++) {
			w[ofs + i * 4 + 3] = uint8_t(MIN(float(w[ofs + i * 4 + 3]) * hi + 0.5f, 255.0f));
		}
	}
	p_image->set_data(p_image->get_width(), p_image->get_height(), true, Image::FORMAT_RGBA8, data);
}

// Mipmaps of the relief that keep its highest point (the nearest to the face) of every 2 x 2 texels: averaging with
// the empty texels (at the bottom) would sink the edges.
void max_mipmaps(Ref<Image> &p_image) {
	ERR_FAIL_COND(p_image->get_format() != Image::FORMAT_RH);
	p_image->generate_mipmaps();
	Vector<uint8_t> data = p_image->get_data();
	uint16_t *h = reinterpret_cast<uint16_t *>(data.ptrw());
	for (int m = 1; m <= p_image->get_mipmap_count(); m++) {
		int64_t pofs, psize, ofs, size;
		int pw, ph, mw, mh;
		p_image->get_mipmap_offset_size_and_dimensions(m - 1, pofs, psize, pw, ph);
		p_image->get_mipmap_offset_size_and_dimensions(m, ofs, size, mw, mh);
		for (int y = 0; y < mh; y++) {
			for (int x = 0; x < mw; x++) {
				float v = 0.0f;
				for (int dy = 0; dy < 2; dy++) {
					for (int dx = 0; dx < 2; dx++) {
						const int sx = MIN(x * 2 + dx, pw - 1);
						const int sy = MIN(y * 2 + dy, ph - 1);
						v = MAX(v, Math::half_to_float(h[pofs / 2 + sy * pw + sx]));
					}
				}
				h[ofs / 2 + y * mw + x] = Math::make_half_float(v);
			}
		}
	}
	p_image->set_data(p_image->get_width(), p_image->get_height(), true, Image::FORMAT_RH, data);
}

} // namespace

void BoxImpostorBaker::dilate_image(Ref<Image> &p_image, const Vector<uint8_t> &p_covered, int p_iterations) {
	dilate(p_image, p_covered, p_iterations);
}

void BoxImpostorBaker::preserve_alpha_coverage_mipmaps(Ref<Image> &p_image) {
	preserve_alpha_coverage(p_image);
}

Ref<ArrayMesh> BoxImpostorBaker::bake(Node3D *p_source) {
	ERR_FAIL_NULL_V(p_source, Ref<ArrayMesh>());
	ERR_FAIL_COND_V_MSG(!p_source->is_inside_tree(), Ref<ArrayMesh>(), "The source of a box impostor must be inside the scene tree.");
	SceneTree *tree = SceneTree::get_singleton();
	ERR_FAIL_NULL_V(tree, Ref<ArrayMesh>());

	// The bounds in the source's space.
	const Transform3D to_source = p_source->get_global_transform().affine_inverse();
	LocalVector<VisualInstance3D *> instances;
	gather_instances(p_source, instances);
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
	ERR_FAIL_COND_V_MSG(!any, Ref<ArrayMesh>(), "A box impostor's source has no visible geometry.");
	bounds = bounds.grow(0.01);

	// The faces: camera basis (r, u, n), right-handed (r x u = n).
	LocalVector<ImpostorFace> faces;
	const Vector3 normals[6] = { Vector3(1, 0, 0), Vector3(-1, 0, 0), Vector3(0, 0, 1), Vector3(0, 0, -1), Vector3(0, 1, 0), Vector3(0, -1, 0) };
	const Vector3 ups[6] = { Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 0, -1), Vector3(0, 0, 1) };
	const Vector3 size = bounds.size;
	float max_depth = 0.0;
	for (int i = 0; i < (bake_bottom ? 6 : 5); i++) {
		ImpostorFace f;
		f.n = normals[i];
		f.u = ups[i];
		f.r = f.u.cross(f.n);
		f.depth = Math::abs(f.n.dot(size));
		f.center = bounds.get_center() + f.n * (f.depth * 0.5);
		max_depth = MAX(max_depth, f.depth);
		faces.push_back(f);
	}

	// Texel density, lowered until the atlas fits.
	float tpm = texels_per_meter;
	int side = 0;
	for (int attempt = 0; attempt < 32; attempt++) {
		Vector<Size2i> sizes;
		for (ImpostorFace &f : faces) {
			const float w = Math::abs(f.r.dot(size));
			const float h = Math::abs(f.u.dot(size));
			f.pixels = Size2i(MAX(int(Math::ceil(w * tpm)), 4), MAX(int(Math::ceil(h * tpm)), 4));
			sizes.push_back(f.pixels);
		}
		const Vector<Point2i> positions = pack_square(sizes, padding, side);
		if (side <= max_size || attempt == 31) {
			for (uint32_t i = 0; i < faces.size(); i++) {
				faces[i].position = positions[i];
			}
			break;
		}
		tpm *= 0.97f * float(max_size) / float(side);
	}
	for (ImpostorFace &f : faces) {
		f.cover = Size2(f.pixels.x / tpm, f.pixels.y / tpm);
	}

	// The scene to photograph: a copy of the source (no scripts) in its own world.
	const float margin = 0.05;
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
	Camera3D *cam = memnew(Camera3D);
	Ref<Environment> env;
	env.instantiate();
	env->set_background(Environment::BG_CLEAR_COLOR);
	env->set_tonemapper(Environment::TONE_MAPPER_LINEAR);
	cam->set_environment(env);
	Node3D *copy = Object::cast_to<Node3D>(p_source->duplicate(Node::DUPLICATE_GROUPS));
	ERR_FAIL_NULL_V(copy, Ref<ArrayMesh>());
	copy->set_transform(Transform3D());
	vp->add_child(copy);
	vp->add_child(cam);
	tree->get_root()->add_child(vp);
	cam->make_current();
	LocalVector<GeometryInstance3D *> geometry;
	gather_geometry(copy, geometry);

	auto photograph = [&](ImpostorFace &f) -> Ref<Image> {
		vp->set_size(f.pixels);
		Transform3D xf;
		xf.basis = Basis(f.r, f.u, f.n); // Columns: the camera's X, Y, Z.
		xf.origin = f.center + f.n * margin;
		cam->set_transform(xf);
		cam->set_orthogonal(f.cover.y, 0.0, f.depth + 2.0 * margin);
		tree->flush_transform_notifications();
		RS::get_singleton()->draw(false);
		RS::get_singleton()->draw(false);
		Ref<Image> img = vp->get_texture()->get_image();
		if (img.is_valid()) {
			img->convert(Image::FORMAT_RGBAF);
		}
		return img;
	};

	// 1. Albedo: unshaded - BaseMaterial3D as unshaded copies (any renderer), other materials through the unshaded
	// debug draw (Forward+; the Mobile renderer has none and draws them lit).
	auto unshaded = [](const Ref<Material> &p_material) -> Ref<Material> {
		Ref<BaseMaterial3D> bm = p_material;
		if (bm.is_null()) {
			return p_material;
		}
		Ref<BaseMaterial3D> copy = bm->duplicate();
		copy->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
		return copy;
	};
	for (GeometryInstance3D *gi : geometry) {
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
	for (ImpostorFace &f : faces) {
		f.color = photograph(f);
	}
	// 2. Normal (view space, RG) and depth below the face (B), every surface opaque.
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
	// Surfaces on the box's faces (within the bounds' growth and 2 cm) at depth 0 exactly: the parallax's linear search
	// takes no step there, whose length could jump off the edge of a wall into the empty texels next to it.
	nd_material->set_shader_parameter("bake_near", margin + 0.03f);
	// The relief's range a little deeper than the box: the farthest geometry stays above its bottom (= nothing there).
	const float relief_range = MAX(max_depth, 0.01f) * 1.02f;
	nd_material->set_shader_parameter("bake_depth", relief_range);
	vp->set_debug_draw(Viewport::DEBUG_DRAW_DISABLED);
	for (GeometryInstance3D *gi : geometry) {
		gi->set_material_override(nd_material);
	}
	for (ImpostorFace &f : faces) {
		f.normal_depth = photograph(f);
	}
	// 3. Windows: the surfaces with transparent materials (glass) white, the rest black.
	bool has_glass = false;
	Ref<StandardMaterial3D> white;
	white.instantiate();
	white->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	white->set_albedo(Color(1, 1, 1));
	Ref<StandardMaterial3D> black;
	black.instantiate();
	black->set_shading_mode(BaseMaterial3D::SHADING_MODE_UNSHADED);
	black->set_albedo(Color(0, 0, 0));
	for (GeometryInstance3D *gi : geometry) {
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(gi);
		if (!mi || mi->get_mesh().is_null()) {
			gi->set_material_override(black);
			continue;
		}
		gi->set_material_override(Ref<Material>());
		for (int s = 0; s < mi->get_mesh()->get_surface_count(); s++) {
			const bool glass = is_glass(mi->get_active_material(s));
			has_glass = has_glass || glass;
			mi->set_surface_override_material(s, glass ? white : black);
		}
	}
	if (has_glass) {
		for (ImpostorFace &f : faces) {
			f.window = photograph(f);
		}
	}
	tree->get_root()->remove_child(vp);
	memdelete(vp);

	// The atlas.
	albedo_image = Image::create_empty(side, side, false, Image::FORMAT_RGBA8);
	normal_image = Image::create_empty(side, side, false, Image::FORMAT_RGB8);
	height_image = Image::create_empty(side, side, false, Image::FORMAT_RH);
	window_image = has_glass ? Image::create_empty(side, side, false, Image::FORMAT_L8) : Ref<Image>();
	normal_image->fill(Color(0.5, 0.5, 1.0));
	Vector<uint8_t> covered;
	covered.resize(side * side);
	covered.fill(0);

	// The mesh, and with it the tangent frame each face's normals are encoded in.
	Ref<SurfaceTool> st;
	st.instantiate();
	st->begin(Mesh::PRIMITIVE_TRIANGLES);
	st->set_custom_format(0, SurfaceTool::CUSTOM_RGBA_FLOAT);
	int vertex = 0;
	for (ImpostorFace &f : faces) {
		const Vector2 a = Vector2(f.position) / float(side);
		const Vector2 b = Vector2(f.position + f.pixels) / float(side);
		const Color rect = Color(a.x, a.y, b.x, b.y);
		const Vector2 sx[4] = { Vector2(-1, 1), Vector2(1, 1), Vector2(1, -1), Vector2(-1, -1) }; // TL, TR, BR, BL.
		for (int k = 0; k < 4; k++) {
			st->set_normal(f.n);
			st->set_uv(Vector2(Math::lerp(a.x, b.x, (sx[k].x + 1.0f) * 0.5f), Math::lerp(a.y, b.y, (1.0f - sx[k].y) * 0.5f)));
			st->set_custom(0, rect);
			st->add_vertex(f.center + f.r * (sx[k].x * f.cover.x * 0.5f) + f.u * (sx[k].y * f.cover.y * 0.5f));
		}
		// Clockwise seen from outside (front faces).
		st->add_index(vertex + 0);
		st->add_index(vertex + 1);
		st->add_index(vertex + 2);
		st->add_index(vertex + 0);
		st->add_index(vertex + 2);
		st->add_index(vertex + 3);
		vertex += 4;
	}
	st->generate_tangents();
	const Array arrays = st->commit_to_arrays();
	const PackedFloat32Array tangents = arrays[Mesh::ARRAY_TANGENT];

	for (uint32_t fi = 0; fi < faces.size(); fi++) {
		ImpostorFace &f = faces[fi];
		ERR_CONTINUE(f.color.is_null() || f.normal_depth.is_null());
		const Vector3 t = Vector3(tangents[fi * 16 + 0], tangents[fi * 16 + 1], tangents[fi * 16 + 2]);
		const float sign = tangents[fi * 16 + 3] < 0.0f ? -1.0f : 1.0f;
		const Vector3 bt = (f.n.cross(t) * sign).normalized(); // As the shaders build it.
		for (int y = 0; y < f.pixels.y; y++) {
			for (int x = 0; x < f.pixels.x; x++) {
				const int ax = f.position.x + x;
				const int ay = f.position.y + y;
				const Color nd = f.normal_depth->get_pixel(x, y);
				if (nd.a < 0.5f) {
					continue; // Nothing there: transparent, at the bottom of the relief.
				}
				covered.write[ay * side + ax] = 1;
				Color c = f.color->get_pixel(x, y);
				c = Color(CLAMP(c.r, 0.0f, 1.0f), CLAMP(c.g, 0.0f, 1.0f), CLAMP(c.b, 0.0f, 1.0f)).linear_to_srgb();
				c.a = 1.0;
				albedo_image->set_pixel(ax, ay, c);
				const Vector2 nv = Vector2(nd.r, nd.g) * 2.0f - Vector2(1, 1);
				const Vector3 nw = (f.r * nv.x + f.u * nv.y + f.n * Math::sqrt(MAX(1.0f - nv.length_squared(), 0.0f))).normalized();
				normal_image->set_pixel(ax, ay, Color(nw.dot(t) * 0.5f + 0.5f, nw.dot(bt) * 0.5f + 0.5f, nw.dot(f.n) * 0.5f + 0.5f));
				height_image->set_pixel(ax, ay, Color(1.0f - CLAMP(nd.b, 0.0f, 1.0f), 0, 0));
				if (window_image.is_valid() && f.window.is_valid()) {
					const float wv = f.window->get_pixel(x, y).r;
					window_image->set_pixel(ax, ay, Color(wv, wv, wv));
				}
			}
		}
	}
	dilate(albedo_image, covered, padding + 2);
	dilate(normal_image, covered, padding + 2);

	relief_depth = relief_range;
	atlas_meters = float(side) / tpm;
	albedo_image->generate_mipmaps();
	preserve_alpha_coverage(albedo_image);
	normal_image->generate_mipmaps(true);
	max_mipmaps(height_image);

	Ref<StandardMaterial3D> material;
	material.instantiate();
	material->set_texture_filter(BaseMaterial3D::TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC);
	material->set_texture(BaseMaterial3D::TEXTURE_ALBEDO, ImageTexture::create_from_image(albedo_image));
	material->set_roughness(0.85);
	material->set_feature(BaseMaterial3D::FEATURE_NORMAL_MAPPING, true);
	material->set_texture(BaseMaterial3D::TEXTURE_NORMAL, ImageTexture::create_from_image(normal_image));
	material->set_feature(BaseMaterial3D::FEATURE_HEIGHT_MAPPING, true);
	material->set_texture(BaseMaterial3D::TEXTURE_HEIGHTMAP, ImageTexture::create_from_image(height_image));
	material->set_heightmap_deep_parallax(true);
	material->set_heightmap_deep_parallax_min_layers(16);
	material->set_heightmap_deep_parallax_max_layers(64);
	material->set_heightmap_impostor(true);
	material->set_heightmap_depth_offset(true);
	// The relief's full depth in UV units (the atlas is square: the same scale along U and V).
	material->set_heightmap_scale(100.0 * relief_range / atlas_meters);
	if (window_image.is_valid()) {
		material->set_texture(BaseMaterial3D::TEXTURE_INTERIOR_MASK, ImageTexture::create_from_image(window_image));
	}
	if (hull) {
		// Mariner G5c: every face's mapping from object space to its atlas UV and relief depth, and its tangent frame
		// (BaseMaterial3D::set_heightmap_impostor_faces()), in the order of `normals`; a face not baked has sign 0.
		PackedVector4Array data;
		data.resize(BaseMaterial3D::IMPOSTOR_FACE_DATA_SIZE);
		data.fill(Vector4());
		for (uint32_t fi = 0; fi < faces.size(); fi++) {
			const ImpostorFace &f = faces[fi];
			const float rect_w = float(f.pixels.x) / float(side);
			const float rect_h = float(f.pixels.y) / float(side);
			const Vector2 rect = Vector2(f.position) / float(side);
			const float kx = rect_w / f.cover.x;
			const float kz = rect_h / f.cover.y;
			const Vector3 rk = f.r * kx;
			const Vector3 uk = -f.u * kz;
			const Vector3 nk = f.n * (-1.0f / relief_range);
			data.write[fi * 4 + 0] = Vector4(rk.x, rk.y, rk.z, rect.x + 0.5f * rect_w - f.center.dot(f.r) * kx);
			data.write[fi * 4 + 1] = Vector4(uk.x, uk.y, uk.z, rect.y + 0.5f * rect_h + f.center.dot(f.u) * kz);
			data.write[fi * 4 + 2] = Vector4(nk.x, nk.y, nk.z, (f.center.dot(f.n) - 0.03f) / relief_range);
			const Vector3 t = Vector3(tangents[fi * 16 + 0], tangents[fi * 16 + 1], tangents[fi * 16 + 2]);
			data.write[fi * 4 + 3] = Vector4(t.x, t.y, t.z, tangents[fi * 16 + 3] < 0.0f ? -1.0f : 1.0f);
		}
		data.write[24] = Vector4(bounds.position.x, bounds.position.y, bounds.position.z, 0.0);
		data.write[25] = Vector4(bounds.get_end().x, bounds.get_end().y, bounds.get_end().z, 0.0);
		material->set_heightmap_impostor_faces(data);
	}

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, arrays, TypedArray<Array>(), Dictionary(), Mesh::ARRAY_CUSTOM_RGBA_FLOAT << Mesh::ARRAY_FORMAT_CUSTOM0_SHIFT);
	mesh->surface_set_material(0, material);
	mesh->set_name(String(p_source->get_name()) + "Impostor");
	return mesh;
}

void BoxImpostorBaker::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_texels_per_meter", "texels"), &BoxImpostorBaker::set_texels_per_meter);
	ClassDB::bind_method(D_METHOD("get_texels_per_meter"), &BoxImpostorBaker::get_texels_per_meter);
	ClassDB::bind_method(D_METHOD("set_max_size", "size"), &BoxImpostorBaker::set_max_size);
	ClassDB::bind_method(D_METHOD("get_max_size"), &BoxImpostorBaker::get_max_size);
	ClassDB::bind_method(D_METHOD("set_padding", "padding"), &BoxImpostorBaker::set_padding);
	ClassDB::bind_method(D_METHOD("get_padding"), &BoxImpostorBaker::get_padding);
	ClassDB::bind_method(D_METHOD("set_bake_bottom", "enable"), &BoxImpostorBaker::set_bake_bottom);
	ClassDB::bind_method(D_METHOD("is_bake_bottom_enabled"), &BoxImpostorBaker::is_bake_bottom_enabled);
	ClassDB::bind_method(D_METHOD("set_hull", "enable"), &BoxImpostorBaker::set_hull);
	ClassDB::bind_method(D_METHOD("is_hull_enabled"), &BoxImpostorBaker::is_hull_enabled);
	ClassDB::bind_method(D_METHOD("bake", "source"), &BoxImpostorBaker::bake);
	ClassDB::bind_method(D_METHOD("get_albedo_image"), &BoxImpostorBaker::get_albedo_image);
	ClassDB::bind_method(D_METHOD("get_normal_image"), &BoxImpostorBaker::get_normal_image);
	ClassDB::bind_method(D_METHOD("get_height_image"), &BoxImpostorBaker::get_height_image);
	ClassDB::bind_method(D_METHOD("get_window_image"), &BoxImpostorBaker::get_window_image);
	ClassDB::bind_method(D_METHOD("get_relief_depth"), &BoxImpostorBaker::get_relief_depth);
	ClassDB::bind_method(D_METHOD("get_atlas_meters"), &BoxImpostorBaker::get_atlas_meters);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "texels_per_meter", PROPERTY_HINT_RANGE, "0.01,64,0.01,or_greater"), "set_texels_per_meter", "get_texels_per_meter");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "max_size", PROPERTY_HINT_RANGE, "64,16384,1"), "set_max_size", "get_max_size");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "padding", PROPERTY_HINT_RANGE, "0,64,1"), "set_padding", "get_padding");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "bake_bottom"), "set_bake_bottom", "is_bake_bottom_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "hull"), "set_hull", "is_hull_enabled");
}
