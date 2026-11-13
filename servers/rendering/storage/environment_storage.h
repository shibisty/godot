/**************************************************************************/
/*  environment_storage.h                                                 */
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

#include "core/math/vector4.h"
#include "core/templates/rid_owner.h"
#include "servers/rendering/rendering_server_enums.h"

class RendererEnvironmentStorage {
public:
	// Mariner F3: color grading in scene-linear light before the tone mapper (white balance, shadows / midtones /
	// highlights, ASC CDL in ACEScct, saturation) and gamut mapping after it.
	struct ColorGrading {
		bool enabled = false;
		float temperature = 6500.0; // Kelvin; the white point that becomes neutral (camera white balance).
		float tint = 0.0; // -1..1, positive towards magenta.
		Color shadows = Color(1, 1, 1);
		Color midtones = Color(1, 1, 1);
		Color highlights = Color(1, 1, 1);
		Vector4 tonal_ranges = Vector4(0.0, 0.333, 0.55, 1.0); // Shadows fade out over x..y, highlights fade in over z..w (linear luminance).
		Vector3 slope = Vector3(1, 1, 1);
		Vector3 offset = Vector3(0, 0, 0);
		Vector3 power = Vector3(1, 1, 1);
		float saturation = 1.0;
		bool gamut_mapping = true;
		// Mariner F3b: night vision (Purkinje shift): towards the rod response below `night_vision_start` scene
		// luminance (before the exposure), fully at `night_vision_end`.
		float night_vision = 0.0;
		Color night_vision_tint = Color(0.70, 0.85, 1.35);
		float night_vision_start = 0.05;
		float night_vision_end = 0.0005;
		// Mariner F3c: channel mixer (rows: the input weights of the output red, green, blue), contrast around the
		// middle grey in ACEScct, vibrance (saturation weighted towards the less saturated colors), per channel curves
		// (power below the mid point, linear slope above it, continuous there).
		Vector3 mixer_red = Vector3(1, 0, 0);
		Vector3 mixer_green = Vector3(0, 1, 0);
		Vector3 mixer_blue = Vector3(0, 0, 1);
		float contrast = 1.0;
		float vibrance = 1.0;
		Vector3 curves_shadow_gamma = Vector3(1, 1, 1);
		Vector3 curves_mid_point = Vector3(0.18, 0.18, 0.18);
		Vector3 curves_highlight_scale = Vector3(1, 1, 1);
	};

	union TonemapParameters {
		// Shader vec4:
		float tonemapper_params[4];

		// Reinhard:
		struct {
			float white_squared;
		};

		// Filmic and ACES:
		struct {
			float white_tonemapped;
		};

		// AgX:
		struct {
			float awp_contrast;
			float awp_toe_a;
			float awp_slope;
			float awp_w;
		};

		// Mariner E23, Gran Turismo 7: display peak in GT7 frame buffer units (1.0 = 100 cd/m^2) and its ICtCp
		// intensity.
		struct {
			float gt7_peak;
			float gt7_peak_intensity;
		};
	};

	// Mariner E23: GT7 curve constants for an output max value (1.0 for SDR; HDR output: peak / paper white).
	static void gt7_tonemap_parameters(float p_output_max_value, float &r_peak, float &r_peak_intensity);

private:
	static RendererEnvironmentStorage *singleton;

	// Environment
public:
	// Mariner G1a: the depth of the scene from above around the camera (Environment.overhead_map_*), for the sky
	// occlusion of the ambient light and reflections (CryEngine HeightMapAO), rain and snow cover.
	struct OverheadMapSettings {
		bool enabled = false;
		float size = 512.0; // Meters, square.
		float height = 256.0; // Meters, from the bottom to the top of the box.
		int resolution = 1024; // Texels per side.
		uint32_t cull_mask = 0xFFFFF;
		float sky_occlusion = 1.0;
		float sky_occlusion_radius = 16.0; // Meters.
		float bias = 0.3; // Meters.
	};

	// The box of the map for a camera: the center snapped to whole texels (no shimmering when the camera moves), half
	// the height above and half below the camera, the top snapped to whole meters. Points above or below the box get
	// no occlusion.
	static void overhead_map_region(const Vector3 &p_camera, float p_size, float p_height, int p_resolution, Vector3 &r_min, Vector3 &r_max);

private:
	struct Environment {
		// Note, we capture and store all environment parameters received from Godot here.
		// Not all renderers support all effects and should just ignore the bits they don't support.

		// Background
		RSE::EnvironmentBG background = RSE::ENV_BG_CLEAR_COLOR;
		RID sky;
		float sky_custom_fov = 0.0;
		Basis sky_orientation;
		Color bg_color;
		float bg_energy_multiplier = 1.0;
		float bg_intensity = 1.0; // Measured in nits or candela/m^2. Default to 1.0 so this doesn't impact rendering when Physical Light Units disabled.
		int canvas_max_layer = 0;
		RSE::EnvironmentAmbientSource ambient_source = RSE::ENV_AMBIENT_SOURCE_BG;
		Color ambient_light;
		float ambient_light_energy = 1.0;
		float ambient_sky_contribution = 1.0;
		RSE::EnvironmentReflectionSource reflection_source = RSE::ENV_REFLECTION_SOURCE_BG;
		int camera_feed_id = 0;

		// Tonemap
		RSE::EnvironmentToneMapper tone_mapper;
		float exposure = 1.0;
		float white = 1.0;
		float tonemap_agx_contrast = 1.25; // Default to approximately Blender's AgX contrast
		float max_value = 1.0;

		// Fog
		bool fog_enabled = false;
		RSE::EnvironmentFogMode fog_mode = RSE::EnvironmentFogMode::ENV_FOG_MODE_EXPONENTIAL;
		Color fog_light_color = Color(0.518, 0.553, 0.608);
		float fog_light_energy = 1.0;
		float fog_sun_scatter = 0.0;
		float fog_density = 0.01;
		float fog_sky_affect = 1.0;
		float fog_height = 0.0;
		float fog_height_density = 0.0; //can be negative to invert effect
		float fog_aerial_perspective = 0.0;

		// Depth Fog
		float fog_depth_curve = 1.0;
		float fog_depth_begin = 10.0;
		float fog_depth_end = 100.0;

		// Volumetric Fog
		bool volumetric_fog_enabled = false;
		float volumetric_fog_density = 0.01;
		Color volumetric_fog_scattering = Color(1, 1, 1);
		Color volumetric_fog_emission = Color(0, 0, 0);
		float volumetric_fog_emission_energy = 0.0;
		float volumetric_fog_anisotropy = 0.2;
		float volumetric_fog_length = 64.0;
		float volumetric_fog_detail_spread = 2.0;
		float volumetric_fog_gi_inject = 1.0;
		float volumetric_fog_ambient_inject = 0.0;
		float volumetric_fog_sky_affect = 1.0;
		bool volumetric_fog_temporal_reprojection = true;
		float volumetric_fog_temporal_reprojection_amount = 0.9;

		// Glow
		bool glow_enabled = false;
		Vector<float> glow_levels;
		float glow_intensity = 0.3;
		float glow_strength = 1.0;
		float glow_bloom = 0.0;
		float glow_mix = 0.01;
		RSE::EnvironmentGlowBlendMode glow_blend_mode = RSE::ENV_GLOW_BLEND_MODE_SCREEN;
		float glow_hdr_bleed_threshold = 1.0;
		float glow_hdr_luminance_cap = 12.0;
		float glow_hdr_bleed_scale = 2.0;
		float glow_map_strength = 0.0f; // 1.0f in GLES3 ??
		RID glow_map;

		// SSR
		bool ssr_enabled = false;
		int ssr_max_steps = 64;
		float ssr_fade_in = 0.15;
		float ssr_fade_out = 2.0;
		float ssr_depth_tolerance = 0.5;

		// SSAO
		bool ssao_enabled = false;
		float ssao_radius = 1.0;
		float ssao_intensity = 2.0;
		float ssao_power = 1.5;
		float ssao_detail = 0.5;
		float ssao_horizon = 0.06;
		float ssao_sharpness = 0.98;
		float ssao_direct_light_affect = 0.0;
		float ssao_ao_channel_affect = 0.0;
		bool ssao_gtao = false; // Mariner E18.
		bool ssao_bent_normals = true;
		ColorGrading color_grading; // Mariner F3.
		float ssao_gtao_thickness = 0.5; // Mariner F1: 0 = horizon-based GTAO.

		// Mariner E20: ReSTIR DI.
		bool restir_enabled = false;
		int restir_candidates = 8;
		int restir_spatial_samples = 3;
		bool restir_temporal = true;
		bool restir_denoise = true;

		// SSIL
		bool ssil_enabled = false;
		float ssil_radius = 5.0;
		float ssil_intensity = 1.0;
		float ssil_sharpness = 0.98;
		float ssil_normal_rejection = 1.0;

		// SDFGI
		bool sdfgi_enabled = false;
		int sdfgi_cascades = 4;
		float sdfgi_min_cell_size = 0.2;
		bool sdfgi_use_occlusion = false;
		float sdfgi_bounce_feedback = 0.5;
		bool sdfgi_read_sky_light = true;
		float sdfgi_energy = 1.0;
		float sdfgi_normal_bias = 1.1;
		float sdfgi_probe_bias = 1.1;
		RSE::EnvironmentSDFGIYScale sdfgi_y_scale = RSE::ENV_SDFGI_Y_SCALE_75_PERCENT;

		// Adjustments
		bool adjustments_enabled = false;
		float adjustments_brightness = 1.0f;
		float adjustments_contrast = 1.0f;
		float adjustments_saturation = 1.0f;
		bool use_1d_color_correction = false;
		RID color_correction;

		// Path tracing (Mariner E5c).
		bool pathtracing_enabled = false;
		int pathtracing_debug_mode = 0;
		int pathtracing_samples_per_pixel = 1;
		int pathtracing_max_bounces = 3;
		RSE::PathtracingDenoiser pathtracing_denoiser = RSE::PT_DENOISER_NONE;
		bool pathtracing_accumulate = true;
		int pathtracing_max_samples = 0;
		float pathtracing_firefly_clamp = 0.0;

		// Dynamic diffuse GI probes (Mariner E12).
		bool ddgi_enabled = false;
		Vector3i ddgi_probe_count = Vector3i(24, 8, 24);
		float ddgi_probe_spacing = 2.0;
		int ddgi_rays_per_probe = 128;
		float ddgi_hysteresis = 0.97;
		float ddgi_energy = 1.0;
		float ddgi_bias = 0.3;
		float ddgi_specular_occlusion = 1.0;
		int ddgi_probes_per_frame = 0; // 0: all.
		bool ddgi_relocation = true;
		float ddgi_sky_energy = 1.0;
		float ddgi_lights_energy = 1.0;
		bool ddgi_sea = false;
		float ddgi_sea_level = 0.0;
		Color ddgi_sea_color = Color(0.02, 0.08, 0.1);

		// Overhead map (Mariner G1a).
		OverheadMapSettings overhead_map;

		float wetness = 0.0; // Mariner G4.
		float snow = 0.0; // Mariner G4b.
	};

	mutable RID_Owner<Environment, true> environment_owner;

public:
	static RendererEnvironmentStorage *get_singleton() { return singleton; }

	RendererEnvironmentStorage();
	virtual ~RendererEnvironmentStorage();

	// Environment
	RID environment_allocate();
	void environment_initialize(RID p_rid);
	void environment_free(RID p_rid);

	bool is_environment(RID p_environment) const {
		return environment_owner.owns(p_environment);
	}

	// Background
	void environment_set_background(RID p_env, RSE::EnvironmentBG p_bg);
	void environment_set_sky(RID p_env, RID p_sky);
	void environment_set_sky_custom_fov(RID p_env, float p_scale);
	void environment_set_sky_orientation(RID p_env, const Basis &p_orientation);
	void environment_set_bg_color(RID p_env, const Color &p_color);
	void environment_set_bg_energy(RID p_env, float p_multiplier, float p_exposure_value);
	void environment_set_canvas_max_layer(RID p_env, int p_max_layer);
	void environment_set_ambient_light(RID p_env, const Color &p_color, RSE::EnvironmentAmbientSource p_ambient = RSE::ENV_AMBIENT_SOURCE_BG, float p_energy = 1.0, float p_sky_contribution = 0.0, RSE::EnvironmentReflectionSource p_reflection_source = RSE::ENV_REFLECTION_SOURCE_BG);
	void environment_set_camera_feed_id(RID p_env, int p_camera_feed_id);
	int environment_get_camera_feed_id(RID p_env) const;

	RSE::EnvironmentBG environment_get_background(RID p_env) const;
	RID environment_get_sky(RID p_env) const;
	float environment_get_sky_custom_fov(RID p_env) const;
	Basis environment_get_sky_orientation(RID p_env) const;
	Color environment_get_bg_color(RID p_env) const;
	float environment_get_bg_energy_multiplier(RID p_env) const;
	float environment_get_bg_intensity(RID p_env) const;
	int environment_get_canvas_max_layer(RID p_env) const;
	RSE::EnvironmentAmbientSource environment_get_ambient_source(RID p_env) const;
	Color environment_get_ambient_light(RID p_env) const;
	float environment_get_ambient_light_energy(RID p_env) const;
	float environment_get_ambient_sky_contribution(RID p_env) const;
	RSE::EnvironmentReflectionSource environment_get_reflection_source(RID p_env) const;

	// Tonemap
	void environment_set_tonemap(RID p_env, RSE::EnvironmentToneMapper p_tone_mapper, float p_exposure, float p_white);
	RSE::EnvironmentToneMapper environment_get_tone_mapper(RID p_env) const;
	float environment_get_exposure(RID p_env) const;
	float environment_get_white(RID p_env, bool p_limit_agx_white, float p_output_max_value) const;
	void environment_set_tonemap_agx_contrast(RID p_env, float p_agx_contrast);
	float environment_get_tonemap_agx_contrast(RID p_env) const;
	TonemapParameters environment_get_tonemap_parameters(RID p_env, bool p_limit_agx_white, float p_output_max_value) const;

	// Fog
	void environment_set_fog(RID p_env, bool p_enable, const Color &p_light_color, float p_light_energy, float p_sun_scatter, float p_density, float p_height, float p_height_density, float p_aerial_perspective, float p_sky_affect, RSE::EnvironmentFogMode p_mode);
	bool environment_get_fog_enabled(RID p_env) const;
	RSE::EnvironmentFogMode environment_get_fog_mode(RID p_env) const;
	Color environment_get_fog_light_color(RID p_env) const;
	float environment_get_fog_light_energy(RID p_env) const;
	float environment_get_fog_sun_scatter(RID p_env) const;
	float environment_get_fog_density(RID p_env) const;
	float environment_get_fog_sky_affect(RID p_env) const;
	float environment_get_fog_height(RID p_env) const;
	float environment_get_fog_height_density(RID p_env) const;
	float environment_get_fog_aerial_perspective(RID p_env) const;

	// Depth Fog
	void environment_set_fog_depth(RID p_env, float p_curve, float p_begin, float p_end);
	float environment_get_fog_depth_curve(RID p_env) const;
	float environment_get_fog_depth_begin(RID p_env) const;
	float environment_get_fog_depth_end(RID p_env) const;

	// Volumetric Fog
	void environment_set_volumetric_fog(RID p_env, bool p_enable, float p_density, const Color &p_albedo, const Color &p_emission, float p_emission_energy, float p_anisotropy, float p_length, float p_detail_spread, float p_gi_inject, bool p_temporal_reprojection, float p_temporal_reprojection_amount, float p_ambient_inject, float p_sky_affect);
	bool environment_get_volumetric_fog_enabled(RID p_env) const;
	float environment_get_volumetric_fog_density(RID p_env) const;
	Color environment_get_volumetric_fog_scattering(RID p_env) const;
	Color environment_get_volumetric_fog_emission(RID p_env) const;
	float environment_get_volumetric_fog_emission_energy(RID p_env) const;
	float environment_get_volumetric_fog_anisotropy(RID p_env) const;
	float environment_get_volumetric_fog_length(RID p_env) const;
	float environment_get_volumetric_fog_detail_spread(RID p_env) const;
	float environment_get_volumetric_fog_gi_inject(RID p_env) const;
	float environment_get_volumetric_fog_sky_affect(RID p_env) const;
	bool environment_get_volumetric_fog_temporal_reprojection(RID p_env) const;
	float environment_get_volumetric_fog_temporal_reprojection_amount(RID p_env) const;
	float environment_get_volumetric_fog_ambient_inject(RID p_env) const;

	// GLOW
	void environment_set_glow(RID p_env, bool p_enable, Vector<float> p_levels, float p_intensity, float p_strength, float p_mix, float p_bloom_threshold, RSE::EnvironmentGlowBlendMode p_blend_mode, float p_hdr_bleed_threshold, float p_hdr_bleed_scale, float p_hdr_luminance_cap, float p_glow_map_strength, RID p_glow_map);
	bool environment_get_glow_enabled(RID p_env) const;
	Vector<float> environment_get_glow_levels(RID p_env) const;
	float environment_get_glow_intensity(RID p_env) const;
	float environment_get_glow_strength(RID p_env) const;
	float environment_get_glow_bloom(RID p_env) const;
	float environment_get_glow_mix(RID p_env) const;
	RSE::EnvironmentGlowBlendMode environment_get_glow_blend_mode(RID p_env) const;
	float environment_get_glow_hdr_bleed_threshold(RID p_env) const;
	float environment_get_glow_hdr_luminance_cap(RID p_env) const;
	float environment_get_glow_hdr_bleed_scale(RID p_env) const;
	float environment_get_glow_map_strength(RID p_env) const;
	RID environment_get_glow_map(RID p_env) const;

	// SSR
	void environment_set_ssr(RID p_env, bool p_enable, int p_max_steps, float p_fade_int, float p_fade_out, float p_depth_tolerance);
	bool environment_get_ssr_enabled(RID p_env) const;
	int environment_get_ssr_max_steps(RID p_env) const;
	float environment_get_ssr_fade_in(RID p_env) const;
	float environment_get_ssr_fade_out(RID p_env) const;
	float environment_get_ssr_depth_tolerance(RID p_env) const;

	// SSAO
	void environment_set_ssao(RID p_env, bool p_enable, float p_radius, float p_intensity, float p_power, float p_detail, float p_horizon, float p_sharpness, float p_light_affect, float p_ao_channel_affect);
	bool environment_get_ssao_enabled(RID p_env) const;
	float environment_get_ssao_radius(RID p_env) const;
	float environment_get_ssao_intensity(RID p_env) const;
	float environment_get_ssao_power(RID p_env) const;
	float environment_get_ssao_detail(RID p_env) const;
	float environment_get_ssao_horizon(RID p_env) const;
	float environment_get_ssao_sharpness(RID p_env) const;
	float environment_get_ssao_direct_light_affect(RID p_env) const;
	float environment_get_ssao_ao_channel_affect(RID p_env) const;
	void environment_set_ssao_gtao(RID p_env, bool p_enable, bool p_bent_normals, float p_thickness = 0.5);
	bool environment_get_ssao_gtao(RID p_env) const;

	// Mariner E20.
	void environment_set_restir(RID p_env, bool p_enable, int p_candidates, int p_spatial_samples, bool p_temporal, bool p_denoise);
	bool environment_get_restir_enabled(RID p_env) const;
	int environment_get_restir_candidates(RID p_env) const;
	int environment_get_restir_spatial_samples(RID p_env) const;
	bool environment_get_restir_temporal(RID p_env) const;
	bool environment_get_restir_denoise(RID p_env) const;
	bool environment_get_ssao_bent_normals(RID p_env) const;
	float environment_get_ssao_gtao_thickness(RID p_env) const;

	// Mariner F3.
	void environment_set_color_grading(RID p_env, bool p_enable, float p_temperature, float p_tint, const Color &p_shadows, const Color &p_midtones, const Color &p_highlights, const Vector4 &p_tonal_ranges, const Vector3 &p_slope, const Vector3 &p_offset, const Vector3 &p_power, float p_saturation, bool p_gamut_mapping);
	ColorGrading environment_get_color_grading(RID p_env) const;
	void environment_set_color_grading_night_vision(RID p_env, float p_amount, const Color &p_tint, float p_start, float p_end); // Mariner F3b.
	void environment_set_color_grading_channel_mixer(RID p_env, const Vector3 &p_red, const Vector3 &p_green, const Vector3 &p_blue); // Mariner F3c.
	void environment_set_color_grading_curves(RID p_env, float p_contrast, float p_vibrance, const Vector3 &p_shadow_gamma, const Vector3 &p_mid_point, const Vector3 &p_highlight_scale); // Mariner F3c.

	// SSIL
	void environment_set_ssil(RID p_env, bool p_enable, float p_radius, float p_intensity, float p_sharpness, float p_normal_rejection);
	bool environment_get_ssil_enabled(RID p_env) const;
	float environment_get_ssil_radius(RID p_env) const;
	float environment_get_ssil_intensity(RID p_env) const;
	float environment_get_ssil_sharpness(RID p_env) const;
	float environment_get_ssil_normal_rejection(RID p_env) const;

	// SDFGI
	void environment_set_sdfgi(RID p_env, bool p_enable, int p_cascades, float p_min_cell_size, RSE::EnvironmentSDFGIYScale p_y_scale, bool p_use_occlusion, float p_bounce_feedback, bool p_read_sky, float p_energy, float p_normal_bias, float p_probe_bias);
	bool environment_get_sdfgi_enabled(RID p_env) const;
	int environment_get_sdfgi_cascades(RID p_env) const;
	float environment_get_sdfgi_min_cell_size(RID p_env) const;
	bool environment_get_sdfgi_use_occlusion(RID p_env) const;
	float environment_get_sdfgi_bounce_feedback(RID p_env) const;
	bool environment_get_sdfgi_read_sky_light(RID p_env) const;
	float environment_get_sdfgi_energy(RID p_env) const;
	float environment_get_sdfgi_normal_bias(RID p_env) const;
	float environment_get_sdfgi_probe_bias(RID p_env) const;
	RSE::EnvironmentSDFGIYScale environment_get_sdfgi_y_scale(RID p_env) const;

	// Adjustment
	void environment_set_adjustment(RID p_env, bool p_enable, float p_brightness, float p_contrast, float p_saturation, bool p_use_1d_color_correction, RID p_color_correction);
	bool environment_get_adjustments_enabled(RID p_env) const;
	float environment_get_adjustments_brightness(RID p_env) const;
	float environment_get_adjustments_contrast(RID p_env) const;
	float environment_get_adjustments_saturation(RID p_env) const;
	bool environment_get_use_1d_color_correction(RID p_env) const;
	RID environment_get_color_correction(RID p_env) const;

	// Path tracing (Mariner E5c)
	void environment_set_pathtracing(RID p_env, bool p_enable, int p_debug_mode, int p_samples_per_pixel, int p_max_bounces, RSE::PathtracingDenoiser p_denoiser);
	void environment_set_pathtracing_accumulation(RID p_env, bool p_accumulate, int p_max_samples, float p_firefly_clamp);
	bool environment_get_pathtracing_enabled(RID p_env) const;
	int environment_get_pathtracing_debug_mode(RID p_env) const;
	int environment_get_pathtracing_samples_per_pixel(RID p_env) const;
	int environment_get_pathtracing_max_bounces(RID p_env) const;
	RSE::PathtracingDenoiser environment_get_pathtracing_denoiser(RID p_env) const;
	bool environment_get_pathtracing_accumulate(RID p_env) const;
	int environment_get_pathtracing_max_samples(RID p_env) const;
	float environment_get_pathtracing_firefly_clamp(RID p_env) const;

	// Dynamic diffuse GI probes (Mariner E12)
	static Vector3i ddgi_clamp_probe_count(const Vector3i &p_count);
	// Probes per axis of a DDGI volume box (DG2): about one per spacing, corners included, within the atlas limits.
	static Vector3i ddgi_volume_probe_count(const Vector3 &p_size, float p_spacing);
	void environment_set_ddgi(RID p_env, bool p_enable, const Vector3i &p_probe_count, float p_probe_spacing, int p_rays_per_probe, float p_hysteresis, float p_energy, float p_bias, float p_specular_occlusion);
	bool environment_get_ddgi_enabled(RID p_env) const;
	Vector3i environment_get_ddgi_probe_count(RID p_env) const;
	float environment_get_ddgi_probe_spacing(RID p_env) const;
	int environment_get_ddgi_rays_per_probe(RID p_env) const;
	float environment_get_ddgi_hysteresis(RID p_env) const;
	float environment_get_ddgi_energy(RID p_env) const;
	float environment_get_ddgi_bias(RID p_env) const;
	float environment_get_ddgi_specular_occlusion(RID p_env) const;
	void environment_set_ddgi_update(RID p_env, int p_probes_per_frame, bool p_relocation);
	int environment_get_ddgi_probes_per_frame(RID p_env) const;
	bool environment_get_ddgi_relocation(RID p_env) const;
	void environment_set_ddgi_channels(RID p_env, float p_sky_energy, float p_lights_energy);
	float environment_get_ddgi_sky_energy(RID p_env) const;
	float environment_get_ddgi_lights_energy(RID p_env) const;
	void environment_set_ddgi_sea(RID p_env, bool p_enable, float p_level, const Color &p_color);
	bool environment_get_ddgi_sea(RID p_env) const;
	float environment_get_ddgi_sea_level(RID p_env) const;
	Color environment_get_ddgi_sea_color(RID p_env) const;

	// Overhead map (Mariner G1a).
	void environment_set_overhead_map(RID p_env, bool p_enable, float p_size, float p_height, int p_resolution, uint32_t p_cull_mask, float p_sky_occlusion, float p_sky_occlusion_radius, float p_bias);
	OverheadMapSettings environment_get_overhead_map(RID p_env) const;

	// Wetness of the world (Mariner G4).
	void environment_set_wetness(RID p_env, float p_wetness);
	float environment_get_wetness(RID p_env) const;
	void environment_set_snow(RID p_env, float p_snow);
	float environment_get_snow(RID p_env) const;
};
