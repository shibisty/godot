/**************************************************************************/
/*  test_mariner_g7c.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_g7c)

#include "core/io/image.h"
#include "scene/resources/3d/mesh_tessellator.h"
#include "scene/resources/3d/primitive_meshes.h"
#include "scene/resources/image_texture.h"
#include "scene/resources/material.h"

// Mariner engine patch G7c: the displacement's mipmap for a tessellated mesh
// (MeshTessellator.get_displacement_texture_lod; set by the editor's Tessellate Mesh).

namespace TestMarinerG7c {

TEST_CASE("[Mariner][G7c] MeshTessellator.get_displacement_texture_lod") {
	Ref<PlaneMesh> plane;
	plane.instantiate(); // 2 x 2 m, UV 0 .. 1.
	Ref<StandardMaterial3D> material;
	material.instantiate();
	CHECK(MeshTessellator::get_displacement_texture_lod(plane, material, 0.1) == doctest::Approx(-1.0)); // No displacement.

	material->set_feature(BaseMaterial3D::FEATURE_DISPLACEMENT, true);
	material->set_texture(BaseMaterial3D::TEXTURE_DISPLACEMENT, ImageTexture::create_from_image(Image::create_empty(256, 256, false, Image::FORMAT_RF)));
	// 256 texels over 2 m: 128 per meter; edges of 1/16 m span 8 texels - the mipmap 3.
	CHECK(MeshTessellator::get_displacement_texture_lod(plane, material, 1.0 / 16.0) == doctest::Approx(3.0));
	// An edge shorter than a texel: the first mipmap.
	CHECK(MeshTessellator::get_displacement_texture_lod(plane, material, 0.001) == doctest::Approx(0.0));
	// The UV scale repeats the texture: twice the texels per meter, one mipmap more.
	material->set_uv1_scale(Vector3(2, 2, 1));
	CHECK(MeshTessellator::get_displacement_texture_lod(plane, material, 1.0 / 16.0) == doctest::Approx(4.0));
}

} // namespace TestMarinerG7c
