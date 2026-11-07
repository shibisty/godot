/**************************************************************************/
/*  test_mariner_e18.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e18)

#include "core/object/class_db.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/effects/gtao.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch E18: GTAO with bent normals (Environment.ssao_gtao / ssao_bent_normals), ported from Spartan
// Engine (MIT, commit cb258a150). Here: the properties and the server call, the quality levels; the image (corners,
// bent normals towards the open sky, specular occlusion, temporal accumulation) on lavapipe by engine/tests/e18/gtao.gd.

namespace TestMarinerE18 {

TEST_CASE("[Mariner][E18] Environment GTAO properties") {
	Ref<Environment> env;
	env.instantiate();
	CHECK_FALSE(env->is_ssao_gtao());
	CHECK(env->is_ssao_bent_normals());
	env->set_ssao_gtao(true);
	env->set_ssao_bent_normals(false);
	CHECK(env->is_ssao_gtao());
	CHECK_FALSE(env->is_ssao_bent_normals());
	CHECK(bool(env->get("ssao_gtao")));
	CHECK_FALSE(bool(env->get("ssao_bent_normals")));
	CHECK(ClassDB::has_method("RenderingServer", "environment_set_ssao_gtao"));
	// The server call goes through without a GPU.
	RS::get_singleton()->environment_set_ssao_gtao(env->get_rid(), true, true);
}

TEST_CASE("[Mariner][E18] GTAO samples per quality level") {
	uint32_t directions = 0;
	uint32_t steps = 0;
	RendererRD::GTAO::quality_samples(RSE::ENV_SSAO_QUALITY_MEDIUM, directions, steps);
	// Medium (the default): 2 slices of 4 steps; low is Spartan's own 2 x 3.
	CHECK(directions == 2);
	CHECK(steps == 4);
	RendererRD::GTAO::quality_samples(RSE::ENV_SSAO_QUALITY_LOW, directions, steps);
	CHECK(directions == 2);
	CHECK(steps == 3);
	uint32_t last = 0;
	for (int q = RSE::ENV_SSAO_QUALITY_VERY_LOW; q <= RSE::ENV_SSAO_QUALITY_ULTRA; q++) {
		RendererRD::GTAO::quality_samples(q, directions, steps);
		CHECK(directions * steps >= last);
		last = directions * steps;
	}
	RendererRD::GTAO::quality_samples(99, directions, steps);
	CHECK(directions == 4);
	CHECK(steps == 8);
}

} // namespace TestMarinerE18
