/**************************************************************************/
/*  mesh_tessellator.h                                                    */
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
#include "scene/resources/mesh.h"

class Material;

// Mariner G7: tessellation as a mesh operation. The triangles are split (longest edge first, without T-junctions)
// until no edge is longer than the target, the new points optionally lifted by Phong tessellation; the LODs are
// simplified on the shape the material's displacement gives the mesh, so the GPU's LOD selection keeps the relief.
class MeshTessellator : public RefCounted {
	GDCLASS(MeshTessellator, RefCounted);

	float target_edge_length = 0.25;
	bool phong_enabled = false;
	float phong_shape = 0.75;
	bool generate_lods = true;
	int max_triangles = 2000000;

protected:
	static void _bind_methods();

public:
	void set_target_edge_length(float p_length);
	float get_target_edge_length() const;

	void set_phong_enabled(bool p_enable);
	bool is_phong_enabled() const;

	void set_phong_shape(float p_shape);
	float get_phong_shape() const;

	void set_generate_lods(bool p_enable);
	bool is_generating_lods() const;

	void set_max_triangles(int p_count);
	int get_max_triangles() const;

	Ref<ArrayMesh> tessellate(const Ref<Mesh> &p_mesh, const Ref<Material> &p_material_override = Ref<Material>()) const;

	// The positions of a surface's vertices as the material's displacement moves them (none: as they are).
	static PackedVector3Array get_displaced_vertices(const Array &p_arrays, const Ref<Material> &p_material);

	// Mariner G7c: the mipmap of the material's displacement texture that has about one texel per edge of that length
	// on the mesh (BaseMaterial3D.displacement_texture_lod): finer texels than vertices would alias. -1 without one.
	static float get_displacement_texture_lod(const Ref<Mesh> &p_mesh, const Ref<Material> &p_material, float p_edge_length);
};
