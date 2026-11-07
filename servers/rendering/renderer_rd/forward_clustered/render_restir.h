/**************************************************************************/
/*  render_restir.h                                                       */
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

// Mariner E20: ReSTIR DI (Environment.restir_enabled) for Forward+ - the direct light of omni and spot lights marked
// Light3D.light_restir, with shadows ray traced on the raytracing scene of the viewport (TLAS, patch E5a), resampled
// in space and time per pixel of the depth prepass and denoised; the opaque pass adds it in place of these lights.
// See shaders/raytracing/restir_di.glsl and restir_denoise.glsl.

#include "core/math/vector3.h"
#include "core/math/vector3i.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/restir_denoise.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/restir_di.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/rt_shadow_denoise.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RendererSceneRenderRD;

namespace RendererSceneRenderImplementation {

class RenderRestirDI {
public:
	enum {
		LIGHT_OMNI = 0,
		LIGHT_SPOT = 1,
		MAX_LIGHTS = 65536,
		MAX_GRID_INDICES = 1 << 22,
		MAX_GRID_AXIS = 128,
		MAX_GRID_CELLS = 1 << 20,
		INVALID_LIGHT = 0xFFFFFFFFu,
		DENOISE_ITERATIONS = 4,
	};

	// Must match RestirLight of restir_di.glsl (std430, 64 bytes).
	struct Light {
		float position[3];
		float inv_range;
		float color[3]; // Linear, × energy as the rasterizer's light buffer (π, exposure).
		float attenuation;
		float direction[3];
		float cos_spot_angle; // Spot: cosine of the cone angle; omni: -1.
		float inv_spot_attenuation;
		float size;
		float specular;
		uint32_t type;
	};
	static_assert(sizeof(Light) == 64, "RestirDI::Light must be 64 bytes for std430.");

	// Must match Params of restir_di.glsl / restir_denoise.glsl (std140).
	struct Params {
		float inv_projection[16];
		float view_to_world[16];
		float prev_world_to_clip[16];
		float grid_origin[4]; // xyz: corner of cell 0, w: cell size.
		int32_t grid_size[4]; // xyz: cells, w: light count.
		int32_t screen_size[2];
		uint32_t frame;
		uint32_t flags; // FLAG_*.
		uint32_t candidates;
		uint32_t spatial_samples;
		float spatial_radius;
		uint32_t prev_light_count;
	};
	static_assert(sizeof(Params) == 256, "RestirDI::Params must be 256 bytes for std140.");

	enum {
		FLAG_TEMPORAL = 1,
		FLAG_HISTORY_VALID = 2,
	};

	// Mariner E20b: ray traced shadows of directional lights. Must match ShadowParams of restir_di.glsl /
	// rt_shadow_denoise.glsl (std140).
	struct ShadowParams {
		float lights[2][4]; // xyz: towards the light (world), w: tangent of the angular radius of its disk.
		uint32_t info[4]; // x: count.
		float range[4]; // x: length of the shadow rays.
	};
	static_assert(sizeof(ShadowParams) == 64, "RestirDI::ShadowParams must be 64 bytes for std140.");
	enum {
		PASS_SHADOW = 3,
	};

	// World grid of light lists over the lit space (the union of the light spheres within p_max_distance of the
	// camera): each cell lists the lights whose range reaches into it, so a surface point samples only lights that can
	// light it; points outside the grid get no light. Size 0: no grid fitted, every point samples all lights.
	struct Grid {
		Vector3 origin; // Corner of cell 0.
		float cell_size = 1.0;
		Vector3i size; // Cells per axis; 0 = no grid.
		LocalVector<uint32_t> cells; // Offset, count per cell.
		LocalVector<uint32_t> indices;
	};
	static void build_grid(const LocalVector<Light> &p_lights, const Vector3 &p_camera, float p_max_distance, Grid &r_grid);
	// For each light index of the previous frame: its index this frame (lights are matched by instance), or
	// INVALID_LIGHT.
	static void build_remap(const LocalVector<uint64_t> &p_previous, const LocalVector<uint64_t> &p_current, LocalVector<uint32_t> &r_remap);

	static bool is_supported();

	// Computes this frame's ReSTIR light of the viewport (after the depth prepass); stores the results in
	// RenderSceneBuffersRD::restir (diffuse, specular). Returns false if nothing was computed.
	bool update(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, RID p_normal_roughness);
	// Mariner E20c: ray traced directional shadows for the late opaque surfaces (water, render_mode opaque_late), after
	// their depth pre-pass (p_opaque_depth: the depth before them); stores the mask in RenderSceneBuffersRD::restir
	// (shadow_late_mask). Needs update() of this frame with such lights. Returns false if nothing was computed.
	bool update_late_shadows(const RenderDataRD *p_render_data, RID p_normal_roughness, RID p_opaque_depth);

	~RenderRestirDI();

private:
	RestirDiShaderRD di_shader;
	RID di_version;
	RID di_shader_rd;
	RID di_pipeline;
	RestirDenoiseShaderRD denoise_shader;
	RID denoise_version;
	RID denoise_shader_rd;
	RID denoise_pipeline;
	RtShadowDenoiseShaderRD shadow_denoise_shader; // Mariner E20b.
	RID shadow_denoise_version;
	RID shadow_denoise_shader_rd;
	RID shadow_denoise_pipeline;
	bool initialized = false;

	LocalVector<Light> lights;
	LocalVector<uint64_t> light_ids;
	LocalVector<uint32_t> remap;
	Grid grid;

	void _initialize();
};

} // namespace RendererSceneRenderImplementation
