/**************************************************************************/
/*  render_ddgi.h                                                         */
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

// Dynamic diffuse global illumination (Mariner E12, Environment.ddgi_enabled) for Forward+: a grid of irradiance probes
// around the camera, updated every frame by rays on the raytracing scene of the viewport (TLAS, patch E5a) with inline
// ray queries, sampled by the scene shader in place of the ambient light. After Majercik et al. 2019 ("Dynamic Diffuse
// Global Illumination with Ray-Traced Irradiance Fields"), written for Godot: see shaders/ddgi_data_inc.glsl for the
// data layout and the grid scrolling.

#include "render_pathtracer.h"

#include "core/math/basis.h"
#include "core/math/vector3i.h"
#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/ddgi_trace.glsl.gen.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/ddgi_update.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RenderSceneBuffersRD;
class RendererSceneRenderRD;

namespace RendererSceneRenderImplementation {

// Must match DDGIData of ddgi_data_inc.glsl (std140).
struct DDGI_Params {
	int32_t grid_min[4]; // xyz: cell of the first probe, w: flags (FLAG_*).
	int32_t probe_count[4]; // xyz: probes per axis, w: rays per probe.
	float spacing[4]; // xyz: spacing per axis (m), w: energy.
	float atlas_inv_size[4]; // 1 / irradiance atlas size, 1 / distance atlas size.
	float misc[4]; // specular occlusion, max distance of the visibility moments, hysteresis, surface bias (m).
	float ray_rotation[16]; // Column major.
	float grid_to_world[16]; // Column major, rigid: grid space (cell * spacing) -> world.
	float world_to_grid[16];
	float channels[4]; // Multiplier of the sky channel, of the lights channel (DG3), unused, unused.
	float sea[4]; // DG4: rgb: color of the light out of the sea body (linear), a: sea level (world y).
};
static_assert(sizeof(DDGI_Params) == 304, "DDGI_Params must be 304 bytes for std140");

// The parameter block of the scene shader and the passes: the world grid, then the local (ship) grid.
struct DDGI_ParamsBlock {
	DDGI_Params volumes[2];
};

class RenderDDGI {
public:
	enum {
		IRRADIANCE_TEXELS = 8,
		DISTANCE_TEXELS = 16,
		FLAG_ACTIVE = 1,
		FLAG_RELOCATION = 2,
		FLAG_SEA = 4,
	};

	// World cell of the first probe of a grid of p_count probes, p_spacing apart, centered on the camera. The grid
	// keeps p_previous while the camera stays within 0.25 cells past the center cell (no scrolling back and forth on a
	// cell boundary).
	static Vector3i grid_min_for(const Vector3 &p_camera, const Vector3i &p_count, float p_spacing, const Vector3i &p_previous, bool p_has_previous);
	static Size2i irradiance_atlas_size(const Vector3i &p_count);
	static Size2i distance_atlas_size(const Vector3i &p_count);
	// Random rotation of the ray directions of a frame (deterministic per frame index).
	static Basis ray_rotation(uint32_t p_frame);
	// Grid of a DDGIVolume3D (DG2): probes per axis, their spacing per axis (the box is covered corner to corner) and
	// the grid space -> world transform (grid space: probe (i, j, k) at (i, j, k) * spacing; centered on the box).
	static void local_volume_layout(const Transform3D &p_transform, const Vector3 &p_size, float p_spacing, Vector3i &r_count, Vector3 &r_spacing, Transform3D &r_grid_to_world);
	static void store_transform(const Transform3D &p_xform, float *r_mat4);
	// World cell of the probe stored in a slot of a grid (as ddgi_slot_to_cell() in ddgi_data_inc.glsl).
	static Vector3i slot_to_cell(uint32_t p_slot, const Vector3i &p_grid_min, const Vector3i &p_count);
	// Probe slots to update this frame: every slot whose cell entered the grid (they must start over now), then the
	// others round robin from r_round_robin until p_budget slots (0: all slots, in order). Returns the number of slots
	// that entered the grid.
	static uint32_t build_slot_list(const Vector3i &p_count, const Vector3i &p_old_grid_min, bool p_has_old, const Vector3i &p_grid_min, uint32_t p_budget, uint32_t &r_round_robin, LocalVector<uint32_t> &r_slots);

	static bool is_supported();
	// Whether the probes of this frame of the viewport can be updated (Forward+ main view with a raytracing scene built
	// this frame). Call after the raytracing scene was built.
	static bool can_update(const RenderDataRD *p_render_data);

private:
	enum TraceVariant {
		TRACE_RADIANCE_2D,
		TRACE_RADIANCE_ARRAY,
		TRACE_MAX,
	};

	DdgiTraceShaderRD trace_shader;
	RID trace_version;
	RID trace_shader_rd[TRACE_MAX];
	RID trace_pipeline[TRACE_MAX];
	DdgiUpdateShaderRD update_shader;
	RID update_version;
	RID update_shader_rd;
	RID update_pipeline;
	bool initialized = false;
	bool use_octmap_array = false;

	RID inactive_params_buffer; // Parameters with the active flag clear (viewports without DDGI).
	LocalVector<RT_LightData> light_data;
	LocalVector<uint32_t> slots;

	struct VolumeSetup {
		bool enabled = false;
		Vector3i count;
		Vector3 spacing;
		int rays = 128;
		int probes_per_frame = 0;
		Transform3D grid_to_world;
		bool follows_camera = false;
	};
	void _ensure_volume_resources(RenderSceneBuffersRD *p_rb, uint32_t p_volume, const VolumeSetup &p_setup);

	void _initialize(RendererSceneRenderRD *p_owner);

public:
	// Updates the probes of the viewport for this frame (rays, then irradiance / distance). Returns false (and the
	// viewport's DDGI is marked inactive) when it can't. p_scene_uniform_buffer: a SceneDataBlock of this frame (sky
	// orientation, ambient light settings); p_radiance_texture: the sky radiance map or RID().
	bool update(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, RID p_scene_uniform_buffer, RID p_radiance_texture);

	// Parameters for the scene shader when a viewport has no DDGI this frame.
	RID get_inactive_params_buffer();

	~RenderDDGI();
};

} // namespace RendererSceneRenderImplementation
