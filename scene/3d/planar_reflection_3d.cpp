/**************************************************************************/
/*  planar_reflection_3d.cpp                                              */
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

#include "planar_reflection_3d.h"

#include "core/config/engine.h"
#include "core/object/class_db.h"
#include "scene/3d/camera_3d.h"
#include "scene/3d/mesh_instance_3d.h"
#include "scene/main/viewport.h"
#include "scene/resources/material.h"
#include "servers/rendering/rendering_server.h"

Camera3D *(*PlanarReflection3D::editor_camera_func)() = nullptr;

bool PlanarReflection3D::is_in_view(Camera3D *p_camera) const {
	ERR_FAIL_NULL_V(p_camera, false);
	const Transform3D gt = get_global_transform();
	const Vector2 h = size * 0.5;
	const Vector3 corners[4] = { gt.xform(Vector3(-h.x, 0, -h.y)), gt.xform(Vector3(h.x, 0, -h.y)), gt.xform(Vector3(h.x, 0, h.y)), gt.xform(Vector3(-h.x, 0, h.y)) };
	// Out of view: all the corners outside one plane of the frustum (the planes face out).
	for (const Plane &plane : p_camera->get_frustum()) {
		bool outside = true;
		for (const Vector3 &c : corners) {
			if (!plane.is_point_over(c)) {
				outside = false;
				break;
			}
		}
		if (outside) {
			return false;
		}
	}
	return true;
}

Transform3D PlanarReflection3D::mirror_transform(const Transform3D &p_camera, const Plane &p_plane) {
	const Vector3 n = p_plane.normal;
	auto reflect_vector = [&](const Vector3 &p_v) { return p_v - n * (2.0 * n.dot(p_v)); };
	Transform3D t;
	t.origin = p_camera.origin - n * (2.0 * p_plane.distance_to(p_camera.origin));
	t.basis.set_column(0, -reflect_vector(p_camera.basis.get_column(0)));
	t.basis.set_column(1, reflect_vector(p_camera.basis.get_column(1)));
	t.basis.set_column(2, reflect_vector(p_camera.basis.get_column(2)));
	return t;
}

void PlanarReflection3D::_assign_surfaces(Node *p_node, bool p_preview) {
	GeometryInstance3D *gi = Object::cast_to<GeometryInstance3D>(p_node);
	if (gi) {
		// Hidden from the reflection camera. The editor's preview changes only what is drawn, not the node or its
		// material (the scene would be saved with them).
		if (p_preview) {
			RS::get_singleton()->instance_set_layer_mask(gi->get_instance(), gi->get_layer_mask() | (1u << (surface_layer - 1)));
		} else if (!gi->get_layer_mask_value(surface_layer)) {
			gi->set_layer_mask_value(surface_layer, true);
		}
		const Ref<Texture2D> texture = get_reflection_texture();
		auto assign = [&](const Ref<Material> &p_material) {
			Ref<BaseMaterial3D> bm = p_material;
			if (bm.is_null() || !bm->get_feature(BaseMaterial3D::FEATURE_PLANAR_REFLECTION) || texture.is_null()) {
				return;
			}
			if (p_preview) {
				RS::get_singleton()->material_set_param(bm->get_rid(), "texture_planar_reflection", texture->get_rid());
			} else if (bm->get_texture(BaseMaterial3D::TEXTURE_PLANAR_REFLECTION) != texture) {
				bm->set_texture(BaseMaterial3D::TEXTURE_PLANAR_REFLECTION, texture);
			}
		};
		assign(gi->get_material_override());
		MeshInstance3D *mi = Object::cast_to<MeshInstance3D>(gi);
		if (mi && mi->get_mesh().is_valid()) {
			for (int i = 0; i < mi->get_mesh()->get_surface_count(); i++) {
				assign(mi->get_active_material(i));
			}
		}
	}
	for (int i = 0; i < p_node->get_child_count(); i++) {
		if (p_node->get_child(i) != viewport) {
			_assign_surfaces(p_node->get_child(i), p_preview);
		}
	}
}

void PlanarReflection3D::_set_active(bool p_active) {
	active = p_active;
	if (viewport) {
		viewport->set_update_mode(p_active ? SubViewport::UPDATE_ALWAYS : SubViewport::UPDATE_DISABLED);
	}
}

void PlanarReflection3D::_update() {
	if (!viewport || !camera || !is_visible_in_tree()) {
		_set_active(false);
		return;
	}
	Viewport *main_viewport = get_viewport();
	Camera3D *main_camera = main_viewport ? main_viewport->get_camera_3d() : nullptr;
	// Mariner G6b: in the editor, the editor's camera (its viewport is the one the surfaces are seen in).
	const bool preview = Engine::get_singleton()->is_editor_hint();
	if (preview) {
		main_camera = editor_camera_func ? editor_camera_func() : nullptr;
		main_viewport = main_camera ? main_camera->get_viewport() : nullptr;
	}
	if (!main_camera || main_camera == camera || !main_viewport) {
		_set_active(false);
		return;
	}
	const Transform3D gt = get_global_transform();
	const Vector3 normal = gt.basis.get_column(1).normalized();
	const Plane plane(normal, gt.origin);
	const Transform3D cam = main_camera->get_global_transform();
	// The camera behind the mirror sees no reflection, nor does one that does not see the mirror (Mariner G6b).
	if (plane.distance_to(cam.origin) <= 0.0 || !is_in_view(main_camera)) {
		_set_active(false);
		return;
	}
	_set_active(true);

	const Size2 main_size = main_viewport->get_visible_rect().size;
	viewport->set_size(Size2i(MAX(int(main_size.x * resolution_scale), 1), MAX(int(main_size.y * resolution_scale), 1)));
	camera->set_global_transform(mirror_transform(cam, plane));
	camera->set_keep_aspect_mode(main_camera->get_keep_aspect_mode());
	const float far_distance = max_distance > 0.0 ? max_distance : main_camera->get_far();
	if (main_camera->get_projection() == Camera3D::PROJECTION_ORTHOGONAL) {
		camera->set_orthogonal(main_camera->get_size(), main_camera->get_near(), far_distance);
	} else {
		camera->set_perspective(main_camera->get_fov(), main_camera->get_near(), far_distance);
	}
	camera->set_cull_mask(main_camera->get_cull_mask() & cull_mask & ~(1u << (surface_layer - 1)));
	RS::get_singleton()->camera_set_clip_plane(camera->get_camera(), true, Plane(normal, plane.d + clip_offset));

	_assign_surfaces(this, preview);
}

void PlanarReflection3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_TREE: {
			viewport = memnew(SubViewport);
			viewport->set_use_hdr_2d(true); // Linear HDR: the scene's light as it is.
			viewport->set_transparent_background(false);
			viewport->set_msaa_3d(Viewport::MSAA_DISABLED);
			viewport->set_screen_space_aa(Viewport::SCREEN_SPACE_AA_DISABLED);
			viewport->set_use_taa(false);
			viewport->set_use_debanding(false);
			viewport->set_scaling_3d_scale(1.0);
			viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
			camera = memnew(Camera3D);
			viewport->add_child(camera);
			add_child(viewport, false, INTERNAL_MODE_FRONT);
			camera->make_current();
			RS::get_singleton()->camera_set_raw_output(camera->get_camera(), true);
			set_process_internal(true);
			_update();
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			_update();
		} break;
		case NOTIFICATION_EXIT_TREE: {
			set_process_internal(false);
			if (viewport) {
				remove_child(viewport);
				viewport->queue_free();
				viewport = nullptr;
				camera = nullptr;
			}
			active = false;
		} break;
	}
}

Ref<Texture2D> PlanarReflection3D::get_reflection_texture() const {
	return viewport ? Ref<Texture2D>(viewport->get_texture()) : Ref<Texture2D>();
}

void PlanarReflection3D::set_size(const Vector2 &p_size) {
	size = p_size.maxf(0.01);
	update_gizmos();
}

Vector2 PlanarReflection3D::get_size() const {
	return size;
}

void PlanarReflection3D::set_resolution_scale(float p_scale) {
	resolution_scale = CLAMP(p_scale, 0.05f, 2.0f);
}

float PlanarReflection3D::get_resolution_scale() const {
	return resolution_scale;
}

void PlanarReflection3D::set_cull_mask(uint32_t p_mask) {
	cull_mask = p_mask;
}

uint32_t PlanarReflection3D::get_cull_mask() const {
	return cull_mask;
}

void PlanarReflection3D::set_surface_layer(int p_layer) {
	surface_layer = CLAMP(p_layer, 1, 20);
}

int PlanarReflection3D::get_surface_layer() const {
	return surface_layer;
}

void PlanarReflection3D::set_clip_offset(float p_offset) {
	clip_offset = p_offset;
}

float PlanarReflection3D::get_clip_offset() const {
	return clip_offset;
}

void PlanarReflection3D::set_max_distance(float p_distance) {
	max_distance = MAX(p_distance, 0.0f);
}

float PlanarReflection3D::get_max_distance() const {
	return max_distance;
}

void PlanarReflection3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("is_in_view", "camera"), &PlanarReflection3D::is_in_view); // Mariner G6b.
	ClassDB::bind_method(D_METHOD("set_size", "size"), &PlanarReflection3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &PlanarReflection3D::get_size);
	ClassDB::bind_method(D_METHOD("set_resolution_scale", "scale"), &PlanarReflection3D::set_resolution_scale);
	ClassDB::bind_method(D_METHOD("get_resolution_scale"), &PlanarReflection3D::get_resolution_scale);
	ClassDB::bind_method(D_METHOD("set_cull_mask", "mask"), &PlanarReflection3D::set_cull_mask);
	ClassDB::bind_method(D_METHOD("get_cull_mask"), &PlanarReflection3D::get_cull_mask);
	ClassDB::bind_method(D_METHOD("set_surface_layer", "layer"), &PlanarReflection3D::set_surface_layer);
	ClassDB::bind_method(D_METHOD("get_surface_layer"), &PlanarReflection3D::get_surface_layer);
	ClassDB::bind_method(D_METHOD("set_clip_offset", "offset"), &PlanarReflection3D::set_clip_offset);
	ClassDB::bind_method(D_METHOD("get_clip_offset"), &PlanarReflection3D::get_clip_offset);
	ClassDB::bind_method(D_METHOD("set_max_distance", "distance"), &PlanarReflection3D::set_max_distance);
	ClassDB::bind_method(D_METHOD("get_max_distance"), &PlanarReflection3D::get_max_distance);
	ClassDB::bind_method(D_METHOD("get_reflection_texture"), &PlanarReflection3D::get_reflection_texture);
	ClassDB::bind_method(D_METHOD("is_reflection_active"), &PlanarReflection3D::is_reflection_active);
	ClassDB::bind_static_method("PlanarReflection3D", D_METHOD("mirror_transform", "camera", "plane"), &PlanarReflection3D::mirror_transform);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "resolution_scale", PROPERTY_HINT_RANGE, "0.05,2,0.01"), "set_resolution_scale", "get_resolution_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mask", PROPERTY_HINT_LAYERS_3D_RENDER), "set_cull_mask", "get_cull_mask");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "surface_layer", PROPERTY_HINT_RANGE, "1,20,1"), "set_surface_layer", "get_surface_layer");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clip_offset", PROPERTY_HINT_RANGE, "-1,1,0.001,or_less,or_greater,suffix:m"), "set_clip_offset", "get_clip_offset");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "max_distance", PROPERTY_HINT_RANGE, "0,10000,0.1,or_greater,suffix:m"), "set_max_distance", "get_max_distance");
}

PlanarReflection3D::PlanarReflection3D() {
	set_process_priority(1000); // After the cameras have moved.
}
