/**************************************************************************/
/*  planar_reflection_3d_gizmo_plugin.cpp                              */
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

#include "planar_reflection_3d_gizmo_plugin.h"

#include "core/math/geometry_3d.h"
#include "editor/editor_undo_redo_manager.h"
#include "editor/scene/3d/node_3d_editor_plugin.h"
#include "editor/settings/editor_settings.h"
#include "scene/3d/planar_reflection_3d.h"

// The editor's camera for the reflection's preview: the last used 3D viewport's.
static Camera3D *_planar_reflection_editor_camera() {
	Node3DEditor *editor = Node3DEditor::get_singleton();
	if (!editor) {
		return nullptr;
	}
	Node3DEditorViewport *viewport = editor->get_last_used_viewport();
	return viewport ? viewport->get_camera_3d() : nullptr;
}

PlanarReflection3DGizmoPlugin::PlanarReflection3DGizmoPlugin() {
	Color gizmo_color = EDITOR_GET("editors/3d_gizmos/gizmo_colors/reflection_probe");
	create_material("planar_reflection_material", gizmo_color);
	gizmo_color.a = 0.1;
	create_material("planar_reflection_solid_material", gizmo_color);
	create_handle_material("handles");
	PlanarReflection3D::editor_camera_func = &_planar_reflection_editor_camera;
}

bool PlanarReflection3DGizmoPlugin::has_gizmo(Node3D *p_spatial) {
	return Object::cast_to<PlanarReflection3D>(p_spatial) != nullptr;
}

String PlanarReflection3DGizmoPlugin::get_gizmo_name() const {
	return "PlanarReflection3D";
}

int PlanarReflection3DGizmoPlugin::get_priority() const {
	return -1;
}

String PlanarReflection3DGizmoPlugin::get_handle_name(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return p_id == 0 ? "Size X" : "Size Z";
}

Variant PlanarReflection3DGizmoPlugin::get_handle_value(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary) const {
	return Object::cast_to<PlanarReflection3D>(p_gizmo->get_node_3d())->get_size();
}

void PlanarReflection3DGizmoPlugin::set_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, Camera3D *p_camera, const Point2 &p_point) {
	PlanarReflection3D *mirror = Object::cast_to<PlanarReflection3D>(p_gizmo->get_node_3d());
	const Transform3D gi = mirror->get_global_transform().affine_inverse();
	const Vector3 ray_from = p_camera->project_ray_origin(p_point);
	const Vector3 ray_dir = p_camera->project_ray_normal(p_point);
	const Vector3 sg[2] = { gi.xform(ray_from), gi.xform(ray_from + ray_dir * 4096) };
	const Vector3 axis = p_id == 0 ? Vector3(1, 0, 0) : Vector3(0, 0, 1);
	Vector3 ra, rb;
	Geometry3D::get_closest_points_between_segments(Vector3(), axis * 4096, sg[0], sg[1], ra, rb);
	float d = ra.dot(axis);
	if (Node3DEditor::get_singleton()->is_snap_enabled()) {
		d = Math::snapped(d, Node3DEditor::get_singleton()->get_translate_snap());
	}
	Vector2 size = mirror->get_size();
	size[p_id] = MAX(d * 2.0f, 0.01f); // Symmetric about the node.
	mirror->set_size(size);
}

void PlanarReflection3DGizmoPlugin::commit_handle(const EditorNode3DGizmo *p_gizmo, int p_id, bool p_secondary, const Variant &p_restore, bool p_cancel) {
	PlanarReflection3D *mirror = Object::cast_to<PlanarReflection3D>(p_gizmo->get_node_3d());
	if (p_cancel) {
		mirror->set_size(p_restore);
		return;
	}
	EditorUndoRedoManager *ur = EditorUndoRedoManager::get_singleton();
	ur->create_action(TTR("Change Planar Reflection Size"));
	ur->add_do_method(mirror, "set_size", mirror->get_size());
	ur->add_undo_method(mirror, "set_size", p_restore);
	ur->commit_action();
}

void PlanarReflection3DGizmoPlugin::redraw(EditorNode3DGizmo *p_gizmo) {
	PlanarReflection3D *mirror = Object::cast_to<PlanarReflection3D>(p_gizmo->get_node_3d());
	p_gizmo->clear();

	const Vector2 h = mirror->get_size() * 0.5;
	const Vector3 corners[4] = { Vector3(-h.x, 0, -h.y), Vector3(h.x, 0, -h.y), Vector3(h.x, 0, h.y), Vector3(-h.x, 0, h.y) };
	Vector<Vector3> lines;
	for (int i = 0; i < 4; i++) {
		lines.push_back(corners[i]);
		lines.push_back(corners[(i + 1) % 4]);
	}
	// The normal (the reflecting side), with an arrowhead.
	const float n = MIN(MIN(h.x, h.y), 1.0f);
	lines.push_back(Vector3());
	lines.push_back(Vector3(0, n, 0));
	lines.push_back(Vector3(0, n, 0));
	lines.push_back(Vector3(n * 0.15, n * 0.8, 0));
	lines.push_back(Vector3(0, n, 0));
	lines.push_back(Vector3(-n * 0.15, n * 0.8, 0));

	p_gizmo->add_lines(lines, get_material("planar_reflection_material", p_gizmo));
	p_gizmo->add_collision_segments(lines);
	if (p_gizmo->is_selected()) {
		p_gizmo->add_solid_box(get_material("planar_reflection_solid_material", p_gizmo), Vector3(h.x * 2.0, 0.001, h.y * 2.0));
	}
	const Vector<Vector3> handles = { Vector3(h.x, 0, 0), Vector3(0, 0, h.y) };
	p_gizmo->add_handles(handles, get_material("handles"));
}
