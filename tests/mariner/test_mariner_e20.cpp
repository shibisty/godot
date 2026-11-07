/**************************************************************************/
/*  test_mariner_e20.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e20)

#include "core/object/class_db.h"
#include "scene/3d/light_3d.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/forward_clustered/render_restir.h"
#include "servers/rendering/renderer_rd/storage_rd/light_storage.h"
#include "servers/rendering/renderer_rd/storage_rd/render_scene_buffers_rd.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch E20: ReSTIR DI - many omni / spot lights with ray traced shadows (Light3D.light_restir,
// Environment.restir_*). Here: the properties, the light grid and the light index remapping between frames; the image
// (same light as the rasterizer where nothing is in the way, shadows of every light, less noise with the denoiser) on
// lavapipe by engine/tests/e20/restir.gd.

namespace TestMarinerE20 {

using RendererSceneRenderImplementation::RenderRestirDI;

static RenderRestirDI::Light make_light(const Vector3 &p_position, float p_range) {
	RenderRestirDI::Light l = {};
	l.position[0] = p_position.x;
	l.position[1] = p_position.y;
	l.position[2] = p_position.z;
	l.inv_range = 1.0f / p_range;
	return l;
}

// Lights listed in the grid cell of a point (the shader's lookup).
static Vector<uint32_t> cell_lights(const RenderRestirDI::Grid &p_grid, const Vector3 &p_point) {
	Vector<uint32_t> out;
	const Vector3 c = ((p_point - p_grid.origin) / p_grid.cell_size).floor();
	const Vector3i ci(int(c.x), int(c.y), int(c.z));
	if (ci.x < 0 || ci.y < 0 || ci.z < 0 || ci.x >= p_grid.size.x || ci.y >= p_grid.size.y || ci.z >= p_grid.size.z) {
		return out;
	}
	const uint32_t cell = (ci.z * p_grid.size.y + ci.y) * p_grid.size.x + ci.x;
	for (uint32_t i = 0; i < p_grid.cells[cell * 2 + 1]; i++) {
		out.push_back(p_grid.indices[p_grid.cells[cell * 2] + i]);
	}
	return out;
}

TEST_CASE("[Mariner][E20] Light3D.light_restir and Environment.restir_*") {
	OmniLight3D *omni = memnew(OmniLight3D);
	CHECK_FALSE(omni->is_restir());
	omni->set_restir(true);
	CHECK(omni->is_restir());
	CHECK(bool(omni->get("light_restir")));
	memdelete(omni);
	CHECK(ClassDB::has_method("RenderingServer", "light_set_restir"));
	CHECK(ClassDB::has_method("RenderingServer", "environment_set_restir"));

	Ref<Environment> env;
	env.instantiate();
	CHECK_FALSE(env->is_restir_enabled());
	CHECK(env->get_restir_candidates() == 8);
	CHECK(env->get_restir_spatial_samples() == 3);
	CHECK(env->is_restir_temporal());
	CHECK(env->is_restir_denoise());
	env->set_restir_candidates(100);
	CHECK(env->get_restir_candidates() == 32);
	env->set_restir_spatial_samples(-1);
	CHECK(env->get_restir_spatial_samples() == 0);
	env->set_restir_enabled(true);
	CHECK(env->is_restir_enabled());
}

TEST_CASE("[Mariner][E20] ReSTIR light grid and light remapping") {
	LocalVector<RenderRestirDI::Light> lights;
	lights.push_back(make_light(Vector3(0, 1, 0), 2.0f)); // 0: at the camera.
	lights.push_back(make_light(Vector3(10, 1, 0), 2.0f)); // 1: 10 m away.
	lights.push_back(make_light(Vector3(0, 1, 0), 30.0f)); // 2: large range.
	RenderRestirDI::Grid grid;
	RenderRestirDI::build_grid(lights, Vector3(0, 14, 0), 1000.0f, grid); // Camera high above (the grid follows the lights).
	REQUIRE(grid.size.x > 0);
	CHECK(grid.cell_size > 0.0f);
	// The grid covers the union of the light spheres: 60 m across (light 2).
	CHECK(grid.size.x * grid.cell_size >= 59.9f);
	// Outside every light: no cell (no light).
	CHECK(cell_lights(grid, Vector3(0, 40, 0)).is_empty());

	// Every light that reaches a point is listed in its cell (no light missed), far lights are not.
	Vector<uint32_t> at_camera = cell_lights(grid, Vector3(0.5, 0.5, 0.5));
	CHECK(at_camera.has(0));
	CHECK(at_camera.has(2));
	CHECK_FALSE(at_camera.has(1));
	Vector<uint32_t> near_far = cell_lights(grid, Vector3(10.5, 1.0, 0.5));
	CHECK(near_far.has(1));
	CHECK(near_far.has(2));
	CHECK_FALSE(near_far.has(0));
	// Points of all lights' ranges on a ring around light 1: always listed.
	for (int i = 0; i < 16; i++) {
		const float a = Math::TAU * i / 16.0f;
		CHECK(cell_lights(grid, Vector3(10.0f + Math::cos(a) * 1.9f, 1.0f, Math::sin(a) * 1.9f)).has(1));
	}

	// No lights: no grid.
	LocalVector<RenderRestirDI::Light> none;
	RenderRestirDI::build_grid(none, Vector3(), 1000.0f, grid);
	CHECK(grid.size == Vector3i());

	// Lights are matched between frames by their instance.
	LocalVector<uint64_t> previous;
	previous.push_back(10);
	previous.push_back(20);
	previous.push_back(30);
	LocalVector<uint64_t> current;
	current.push_back(30);
	current.push_back(10);
	current.push_back(40);
	LocalVector<uint32_t> remap;
	RenderRestirDI::build_remap(previous, current, remap);
	REQUIRE(remap.size() == 3);
	CHECK(remap[0] == 1);
	CHECK(remap[1] == uint32_t(RenderRestirDI::INVALID_LIGHT));
	CHECK(remap[2] == 0);
}

// Mariner E20b: ray traced shadows of directional lights (DirectionalLight3D.light_restir). The image (penumbra as
// the physics gives it, contact hardening, at any distance, steady) on lavapipe by engine/tests/e20/sun_shadow.gd.
TEST_CASE("[Mariner][E20b] Ray traced directional shadows: parameters and the light property") {
	// std140 layout shared with restir_di.glsl and rt_shadow_denoise.glsl.
	CHECK(sizeof(RenderRestirDI::ShadowParams) == 64);
	CHECK(offsetof(RenderRestirDI::ShadowParams, lights) == 0);
	CHECK(offsetof(RenderRestirDI::ShadowParams, info) == 32);
	CHECK(offsetof(RenderRestirDI::ShadowParams, range) == 48);
	CHECK(int(RenderRestirDI::PASS_SHADOW) == 3);
	// Two lights (the sun and the moon): the r and g channels of the mask.
	CHECK(int(RendererRD::LightStorage::MAX_DIRECTIONAL_RT_SHADOWS) == 2);

	DirectionalLight3D *sun = memnew(DirectionalLight3D);
	CHECK_FALSE(sun->is_restir());
	sun->set_restir(true);
	CHECK(sun->is_restir());
	CHECK(bool(sun->get("light_restir")));
	memdelete(sun);
}

// Mariner E20c: ray traced directional shadows for the late opaque surfaces (water, render_mode opaque_late) - their
// own surfaces, history and mask, computed after their depth pre-pass. The image (shadow on a late plane beyond the
// shadow distance, penumbra as expected) on lavapipe by engine/tests/e20/sun_shadow.gd.
TEST_CASE("[Mariner][E20c] Ray traced shadows of the late opaque pass: state") {
	RenderSceneBuffersRD::Restir st;
	CHECK(st.shadow_late_mask.is_null()); // No mask until computed this frame: the late pass keeps the shadow maps.
	CHECK(st.late_shadow_params_buffer.is_null());
	CHECK_FALSE(st.late_shadow_history_valid);
	CHECK(st.late_frame == UINT64_MAX);
	CHECK(st.shadow_count == 0);
	// The late pass reads the same ShadowParams layout (its own copy with its own history flag).
	CHECK(sizeof(RenderRestirDI::ShadowParams) == 64);
}

// Mariner E20d: the traced shadow only beyond the shadow maps (DirectionalLight3D.restir_shadow_mode) - for near casters
// that are not in the raytracing scene. The image (map shadow near, traced shadow beyond max_distance) on lavapipe by
// engine/tests/e20/sun_shadow.gd.
TEST_CASE("[Mariner][E20d] Ray traced directional shadows beyond the shadow maps: the light property") {
	DirectionalLight3D *sun = memnew(DirectionalLight3D);
	CHECK(sun->get_restir_shadow_mode() == DirectionalLight3D::RESTIR_SHADOW_REPLACE);
	sun->set_restir_shadow_mode(DirectionalLight3D::RESTIR_SHADOW_BEYOND_SHADOW_MAPS);
	CHECK(sun->get_restir_shadow_mode() == DirectionalLight3D::RESTIR_SHADOW_BEYOND_SHADOW_MAPS);
	CHECK(int(sun->get("restir_shadow_mode")) == 1);
	sun->set("restir_shadow_mode", 0);
	CHECK(sun->get_restir_shadow_mode() == DirectionalLight3D::RESTIR_SHADOW_REPLACE);
	memdelete(sun);
	CHECK(ClassDB::has_method("RenderingServer", "light_directional_set_restir_shadow_mode"));
	CHECK(int(DirectionalLight3D::RESTIR_SHADOW_BEYOND_SHADOW_MAPS) == 1);
}

} // namespace TestMarinerE20
