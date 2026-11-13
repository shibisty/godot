/**************************************************************************/
/*  box_impostor_baker.h                                                  */
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

// Mariner G5b: box impostors. A model (a building, a quay, a warehouse) seen from far away is drawn as its bounding box:
// every face carries what an orthographic camera saw of the model from that side - color, normals and the depth below
// the face - and the material's deep parallax in impostor mode (BaseMaterial3D.heightmap_impostor) traces the view ray
// into that relief. Right parallax and silhouette from any angle, depth for intersections and shadows, and the light,
// wetness and fog of an ordinary material, for a dozen triangles.
class BoxImpostorBaker : public RefCounted {
	GDCLASS(BoxImpostorBaker, RefCounted);

	float texels_per_meter = 8.0;
	int max_size = 2048;
	int padding = 4;
	bool bake_bottom = false;
	bool hull = true; // Mariner G5c.

	Ref<Image> albedo_image;
	Ref<Image> normal_image;
	Ref<Image> height_image;
	Ref<Image> window_image;
	float relief_depth = 0.0;
	float atlas_meters = 0.0;

protected:
	static void _bind_methods();

public:
	void set_texels_per_meter(float p_texels);
	float get_texels_per_meter() const;
	void set_max_size(int p_size);
	int get_max_size() const;
	void set_padding(int p_padding);
	int get_padding() const;
	void set_bake_bottom(bool p_enable);
	bool is_bake_bottom_enabled() const;
	void set_hull(bool p_enable);
	bool is_hull_enabled() const;

	Ref<ArrayMesh> bake(Node3D *p_source);

	Ref<Image> get_albedo_image() const { return albedo_image; }
	Ref<Image> get_normal_image() const { return normal_image; }
	Ref<Image> get_height_image() const { return height_image; }
	Ref<Image> get_window_image() const { return window_image; }
	float get_relief_depth() const { return relief_depth; }
	float get_atlas_meters() const { return atlas_meters; }

	// Packs rectangles of the given sizes (each with p_padding around it) into a square of side r_side, rows of
	// rectangles sorted by height. Returns the positions (of the rectangles, inside their padding).
	static Vector<Point2i> pack_square(const Vector<Size2i> &p_sizes, int p_padding, int &r_side);

	// Mariner G5e: shared with OctahedralImpostorBaker. Fills the empty texels near covered ones with their neighbours'
	// average; scales the alpha of an RGBA8 image's mipmaps to keep the coverage of the alpha scissor (0.5).
	static void dilate_image(Ref<Image> &p_image, const Vector<uint8_t> &p_covered, int p_iterations);
	static void preserve_alpha_coverage_mipmaps(Ref<Image> &p_image);
};
