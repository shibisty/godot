/**************************************************************************/
/*  render_pathtracer.h                                                   */
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

// Path tracer (Mariner E5c) for Forward+: replaces the opaque passes of a viewport whose environment has
// pathtracing_enabled, on the raytracing scene (TLAS) of patch E5a. Ported from NVIDIA's Godot fork (NVIDIA-RTX/godot,
// nvidia-pt-dlss: RenderForwardClusteredPT, raygen / hit shaders, lights, BRDF) as one compute shader with inline ray
// queries; accumulates samples over frames while the camera, scene and settings stay the same.

#include "core/templates/local_vector.h"
#include "servers/rendering/renderer_rd/shaders/raytracing/pathtracer.glsl.gen.h"
#include "servers/rendering/rendering_device.h"

class RenderDataRD;
class RendererSceneRenderRD;

namespace RendererSceneRenderImplementation {

// Light types (match RT_LIGHT_TYPE_* in raytracing_lights_inc.glsl).
enum RTLightType : uint32_t {
	RT_LIGHT_TYPE_OMNI = 0,
	RT_LIGHT_TYPE_DIRECTIONAL = 1,
	RT_LIGHT_TYPE_SPOT = 3,
};

// Must match GLSL RTLightData (std430, 80 bytes). From the fork.
struct alignas(16) RT_LightData {
	float position[3]; // World position (omni/spot) or direction (directional, normalized).
	uint32_t type;
	float emission[3];
	float radius;
	float attenuation;
	float inv_max_range; // 1.0/range, or -1.0 for infinite.
	float max_range_squared; // range*range, or 0.0 for infinite.
	float specular_amount;
	float indirect_energy;
	float inv_spot_attenuation;
	float cos_spot_angle;
	float _pad0;
	float spot_direction[3];
	float _pad1;
};
static_assert(sizeof(RT_LightData) == 80, "RT_LightData must be 80 bytes for std430");

// Must match the GLSL `Params` uniform block of pathtracer.glsl (std140).
struct PT_Params {
	uint32_t counts[4]; // light count, frame index, accumulated samples, flags.
	uint32_t config[4]; // samples per pixel, max bounces, debug mode, sun light index.
	float config2[4]; // firefly clamp, ray length, 1 / volumetric fog length, volumetric fog detail spread.
	float ambient[4];
	float size[2];
	float inv_size[2];
};
static_assert(sizeof(PT_Params) == 80, "PT_Params must be 80 bytes for std140");

class RenderPathtracer {
public:
	enum {
		RT_LIGHTS_MAX = 64,

		PT_FLAG_TRACE = 1 << 0,
		PT_FLAG_FOG = 1 << 1,
		PT_FLAG_DEPTH_FOG = 1 << 2,
		PT_FLAG_DIELECTRICS = 1 << 3,
		PT_FLAG_RADIANCE = 1 << 4,
		PT_FLAG_WRITE_DEPTH = 1 << 5,
		PT_FLAG_VOLUMETRIC_FOG = 1 << 6,
	};

	// What the accumulation of a viewport depends on, besides the scene (TLAS hash) and the lights.
	struct Settings {
		int debug_mode = 0;
		int max_bounces = 3;
		float firefly_clamp = 0.0;
		bool accumulate = true;
		float exposure_normalization = 1.0;
		bool fog = false;

		uint32_t hash() const;
	};

	// Whether the camera moved: transforms and projections are compared with a tolerance, the TAA jitter terms of
	// the projection (Projection::add_jitter_offset) are ignored.
	static bool camera_changed(const Transform3D &p_a, const Transform3D &p_b, const Projection &p_proj_a, const Projection &p_proj_b);
	// Accumulation step of a frame: how many samples to trace (0 = only composite the existing image).
	static uint32_t samples_to_trace(uint32_t p_accumulated, uint32_t p_samples_per_pixel, uint32_t p_max_samples, bool p_accumulate);

	struct Inputs {
		RID scene_uniform_buffer; // SceneDataBlock of the opaque pass.
		RID radiance_texture; // Sky radiance (octahedral), RID() if none.
		RID depth_framebuffer; // Depth-only framebuffer the primary depth is copied into.
		bool fog_enabled = false;
		bool depth_fog = false;
		Color ambient_light; // Linear ambient light used when there is no radiance map.
		// Volumetric fog of this frame (froxel volume, rgb in-scattered light, a transmittance), composited over the
		// traced pixels like the opaque pass does; RID() if the environment has none.
		RID volumetric_fog_texture;
		float volumetric_fog_inv_length = 1.0;
		float volumetric_fog_detail_spread = 1.0;
	};

private:
	enum ShaderVariant {
		VARIANT_RADIANCE_2D,
		VARIANT_RADIANCE_ARRAY,
		VARIANT_MAX,
	};

	PathtracerShaderRD shader;
	RID shader_version;
	RID shader_rd[VARIANT_MAX];
	RID pipeline[VARIANT_MAX];
	bool initialized = false;
	bool use_octmap_array = false;

	LocalVector<RT_LightData> light_data;

	void _initialize(RendererSceneRenderRD *p_owner);
	uint32_t _gather_lights(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, uint32_t &r_sun_index);

public:
	static bool is_supported();
	// Whether this frame of the viewport can be path traced (Forward+ main view, raytracing scene built this frame,
	// no MSAA / multiview, storage-capable RGBA16F internal texture). Call after the raytracing scene was built.
	static bool can_render(const RenderDataRD *p_render_data);

	// Traces (or keeps accumulating) the opaque scene of this frame into the internal color texture, composited over
	// what is there (the sky drawn by the caller), and writes its depth to p_inputs.depth_framebuffer. Returns false
	// when the viewport can't be path traced (no raytracing scene this frame, MSAA, multiview, formats), then nothing
	// was written and the caller renders the opaque passes as usual.
	bool render(RendererSceneRenderRD *p_owner, const RenderDataRD *p_render_data, const Inputs &p_inputs);

	~RenderPathtracer();
};

} // namespace RendererSceneRenderImplementation
