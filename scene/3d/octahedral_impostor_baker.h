/**************************************************************************/
/*  octahedral_impostor_baker.h                                           */
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

#include "core/object/ref_counted.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/mesh.h"

class Node3D;

// Mariner G5e: octahedral impostors with depth, for far models whose shape a box does not hold - cranes, masts, ships,
// lattice towers. The model is photographed orthographically from frames x frames directions spread over the sphere (or
// the upper hemisphere) by the octahedral map; every photo keeps the color, the object-space normal and the depth. The
// impostor is one quad facing the camera: the shader blends the 3 photos nearest to the view direction, each traced
// with parallax along the view ray into its own depth, so the silhouette, the light and the depth (shadows, the
// intersections with water and terrain) follow the model between the photographed directions.
class OctahedralImpostorBaker : public RefCounted {
	GDCLASS(OctahedralImpostorBaker, RefCounted);

	int frames = 12;
	int frame_size = 128;
	bool hemisphere = false;
	int padding = 2;

	Ref<Image> albedo_image;
	Ref<Image> normal_depth_image;
	Vector3 center;
	float radius = 0.0;

protected:
	static void _bind_methods();

public:
	void set_frames(int p_frames);
	int get_frames() const;
	void set_frame_size(int p_size);
	int get_frame_size() const;
	void set_hemisphere(bool p_enable);
	bool is_hemisphere() const;
	void set_padding(int p_padding);
	int get_padding() const;

	// The direction toward the camera of the frame at the grid point p_grid (0 to frames - 1 on both axes).
	static Vector3 frame_direction(const Vector2 &p_grid, int p_frames, bool p_hemisphere);
	// The grid position (0 to frames - 1) of a direction; the inverse of frame_direction().
	static Vector2 direction_to_grid(const Vector3 &p_direction, int p_frames, bool p_hemisphere);
	// The frame's camera basis: X right, Y up, Z = the direction toward the camera.
	static Basis frame_basis(const Vector3 &p_direction);

	Ref<ArrayMesh> bake(Node3D *p_source);
	static Ref<Shader> get_shader();

	Ref<Image> get_albedo_image() const { return albedo_image; }
	Ref<Image> get_normal_depth_image() const { return normal_depth_image; }
	Vector3 get_center() const { return center; }
	float get_radius() const { return radius; }
};
