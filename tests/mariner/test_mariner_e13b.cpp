/**************************************************************************/
/*  test_mariner_e13b.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e13b)

#include "core/object/class_db.h"

// Mariner engine patch E13b: the frame's depth pyramid for effects - RenderSceneBuffersRD.get_hiz() (RG32F, r the
// farthest raw depth, g the nearest, every mipmap) and get_hiz_mipmap_count(). The pyramid itself needs a GPU:
// engine/tests/g/hiz.gd.

namespace TestMarinerE13b {

TEST_CASE("[Mariner][E13b] RenderSceneBuffersRD.get_hiz") {
	CHECK(ClassDB::has_method("RenderSceneBuffersRD", "get_hiz"));
	CHECK(ClassDB::has_method("RenderSceneBuffersRD", "get_hiz_mipmap_count"));
	MethodBind *m = ClassDB::get_method("RenderSceneBuffersRD", "get_hiz");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 1);
	CHECK(m->get_default_argument_count() == 1);
}

} // namespace TestMarinerE13b
