/**************************************************************************/
/*  planar_reflection_3d.h                                                */
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

#pragma once

#include "scene/3d/node_3d.h"

class Camera3D;
class SubViewport;
class GeometryInstance3D;

// Mariner G6: a planar reflection (mirrors, glass, a wet deck, calm water without raytracing). The plane is the node's
// XZ plane (normal +Y); every frame a camera mirrored in it renders the scene into a texture (linear HDR, without tone
// mapping or camera effects, clipped at the plane), and the BaseMaterial3D of the node's GeometryInstance3D children
// with planar_reflection_enabled read it (REFLECTION in the scene shaders). The children are hidden from the
// reflection camera by `surface_layer`.
class PlanarReflection3D : public Node3D {
	GDCLASS(PlanarReflection3D, Node3D);

	Vector2 size = Vector2(10, 10);
	float resolution_scale = 0.5;
	uint32_t cull_mask = 0xFFFFF;
	int surface_layer = 20;
	float clip_offset = 0.0;
	float max_distance = 0.0;

	SubViewport *viewport = nullptr;
	Camera3D *camera = nullptr;
	bool active = false;

	void _update();
	void _set_active(bool p_active);
	void _assign_surfaces(Node *p_node, bool p_preview);

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_size(const Vector2 &p_size);
	Vector2 get_size() const;
	void set_resolution_scale(float p_scale);
	float get_resolution_scale() const;
	void set_cull_mask(uint32_t p_mask);
	uint32_t get_cull_mask() const;
	void set_surface_layer(int p_layer);
	int get_surface_layer() const;
	void set_clip_offset(float p_offset);
	float get_clip_offset() const;
	void set_max_distance(float p_distance);
	float get_max_distance() const;

	Ref<Texture2D> get_reflection_texture() const;
	bool is_reflection_active() const { return active; }

	// The camera mirrored in a plane, with its X axis turned so that the basis stays right-handed (the image is then
	// mirrored along X: the material reads it at (1 - u, v)).
	static Transform3D mirror_transform(const Transform3D &p_camera, const Plane &p_plane);

	// Mariner G6b: the editor's camera (set by the editor), for the preview in the editor's 3D viewports.
	static Camera3D *(*editor_camera_func)();

	// Whether the mirror's rectangle (size, in the node's XZ plane) is in a camera's view.
	bool is_in_view(Camera3D *p_camera) const;

	PlanarReflection3D();
};
