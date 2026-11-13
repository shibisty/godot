/**************************************************************************/
/*  environment_storage.cpp                                               */
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

#include "environment_storage.h"

#ifdef DEBUG_ENABLED
#include "core/os/os.h"
#endif

// Storage

RendererEnvironmentStorage *RendererEnvironmentStorage::singleton = nullptr;

RendererEnvironmentStorage::RendererEnvironmentStorage() {
	singleton = this;
}

RendererEnvironmentStorage::~RendererEnvironmentStorage() {
	singleton = nullptr;
}

// Environment

RID RendererEnvironmentStorage::environment_allocate() {
	return environment_owner.allocate_rid();
}

void RendererEnvironmentStorage::environment_initialize(RID p_rid) {
	environment_owner.initialize_rid(p_rid, Environment());
}

void RendererEnvironmentStorage::environment_free(RID p_rid) {
	environment_owner.free(p_rid);
}

// Background

void RendererEnvironmentStorage::environment_set_background(RID p_env, RSE::EnvironmentBG p_bg) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->background = p_bg;
}

void RendererEnvironmentStorage::environment_set_sky(RID p_env, RID p_sky) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->sky = p_sky;
}

void RendererEnvironmentStorage::environment_set_sky_custom_fov(RID p_env, float p_scale) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->sky_custom_fov = p_scale;
}

void RendererEnvironmentStorage::environment_set_sky_orientation(RID p_env, const Basis &p_orientation) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->sky_orientation = p_orientation;
}

void RendererEnvironmentStorage::environment_set_bg_color(RID p_env, const Color &p_color) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->bg_color = p_color;
}

void RendererEnvironmentStorage::environment_set_bg_energy(RID p_env, float p_multiplier, float p_intensity) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->bg_energy_multiplier = p_multiplier;
	env->bg_intensity = p_intensity;
}

void RendererEnvironmentStorage::environment_set_canvas_max_layer(RID p_env, int p_max_layer) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->canvas_max_layer = p_max_layer;
}

void RendererEnvironmentStorage::environment_set_ambient_light(RID p_env, const Color &p_color, RSE::EnvironmentAmbientSource p_ambient, float p_energy, float p_sky_contribution, RSE::EnvironmentReflectionSource p_reflection_source) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->ambient_light = p_color;
	env->ambient_source = p_ambient;
	env->ambient_light_energy = p_energy;
	env->ambient_sky_contribution = p_sky_contribution;
	env->reflection_source = p_reflection_source;
}

RSE::EnvironmentBG RendererEnvironmentStorage::environment_get_background(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::ENV_BG_CLEAR_COLOR);
	return env->background;
}

RID RendererEnvironmentStorage::environment_get_sky(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RID());
	return env->sky;
}

float RendererEnvironmentStorage::environment_get_sky_custom_fov(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->sky_custom_fov;
}

Basis RendererEnvironmentStorage::environment_get_sky_orientation(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Basis());
	return env->sky_orientation;
}

Color RendererEnvironmentStorage::environment_get_bg_color(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Color());
	return env->bg_color;
}

float RendererEnvironmentStorage::environment_get_bg_energy_multiplier(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->bg_energy_multiplier;
}

float RendererEnvironmentStorage::environment_get_bg_intensity(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->bg_intensity;
}

int RendererEnvironmentStorage::environment_get_canvas_max_layer(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0);
	return env->canvas_max_layer;
}

RSE::EnvironmentAmbientSource RendererEnvironmentStorage::environment_get_ambient_source(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::ENV_AMBIENT_SOURCE_BG);
	return env->ambient_source;
}

Color RendererEnvironmentStorage::environment_get_ambient_light(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Color());
	return env->ambient_light;
}

float RendererEnvironmentStorage::environment_get_ambient_light_energy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ambient_light_energy;
}

float RendererEnvironmentStorage::environment_get_ambient_sky_contribution(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ambient_sky_contribution;
}

RSE::EnvironmentReflectionSource RendererEnvironmentStorage::environment_get_reflection_source(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::ENV_REFLECTION_SOURCE_BG);
	return env->reflection_source;
}

void RendererEnvironmentStorage::environment_set_camera_feed_id(RID p_env, int p_camera_feed_id) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->camera_feed_id = p_camera_feed_id;
}

int RendererEnvironmentStorage::environment_get_camera_feed_id(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, -1);
	return env->camera_feed_id;
}

// Tonemap

void RendererEnvironmentStorage::environment_set_tonemap(RID p_env, RSE::EnvironmentToneMapper p_tone_mapper, float p_exposure, float p_white) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->exposure = p_exposure;
	env->tone_mapper = p_tone_mapper;
	env->white = p_white;
}

RSE::EnvironmentToneMapper RendererEnvironmentStorage::environment_get_tone_mapper(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::ENV_TONE_MAPPER_LINEAR);
	return env->tone_mapper;
}

float RendererEnvironmentStorage::environment_get_exposure(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->exposure;
}

float RendererEnvironmentStorage::environment_get_white(RID p_env, bool p_limit_agx_white, float p_output_max_value) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);

	// Glow with screen blend mode does not work when white < 1.0, so make sure
	// it is at least 1.0 for all tonemappers:
	if (env->tone_mapper == RSE::ENV_TONE_MAPPER_LINEAR || env->tone_mapper == RSE::ENV_TONE_MAPPER_GT7) {
		// GT7 maps to the display peak by itself; white is not a parameter of it.
		return p_output_max_value;
	} else if (env->tone_mapper == RSE::ENV_TONE_MAPPER_FILMIC || env->tone_mapper == RSE::ENV_TONE_MAPPER_ACES) {
		// Filmic and ACES only support SDR; their white is stable regardless
		// of output_max_value.
		return MAX(1.0, env->white);
	} else if (env->tone_mapper == RSE::ENV_TONE_MAPPER_AGX) {
		// AgX works best with a high white. 2.0 is the minimum required for
		// good behavior with Mobile rendering method.
		if (p_limit_agx_white) {
			return 2.0;
		} else {
			float agx_white = MAX(2.0, env->white);
			// Instead of constraining by matching the output_max_value, constrain
			// by multiplying to ensure the desired non-uniform scaling behavior
			// is maintained in the shoulder.
			return agx_white * p_output_max_value;
		}
	} else { // Reinhard
		// The Reinhard tonemapper is not designed to have a white parameter
		// that is less than the output max value. This is especially important
		// in the variable Extended Dynamic Range (EDR) paradigm where the
		// output max value may change to be greater or less than the white
		// parameter, depending on the available dynamic range.
		return MAX(p_output_max_value, env->white);
	}
}

void RendererEnvironmentStorage::environment_set_tonemap_agx_contrast(RID p_env, float p_agx_contrast) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->tonemap_agx_contrast = p_agx_contrast;
}

float RendererEnvironmentStorage::environment_get_tonemap_agx_contrast(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->tonemap_agx_contrast;
}

RendererEnvironmentStorage::TonemapParameters RendererEnvironmentStorage::environment_get_tonemap_parameters(RID p_env, bool p_limit_agx_white, float p_output_max_value) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, TonemapParameters());

	float white = environment_get_white(p_env, p_limit_agx_white, p_output_max_value);
	TonemapParameters tonemap_parameters = TonemapParameters();

	if (env->tone_mapper == RSE::ENV_TONE_MAPPER_LINEAR) {
		// Linear has no tonemapping parameters
	} else if (env->tone_mapper == RSE::ENV_TONE_MAPPER_REINHARD) {
		tonemap_parameters.white_squared = (white * white) / p_output_max_value;
	} else if (env->tone_mapper == RSE::ENV_TONE_MAPPER_FILMIC) {
		// These constants must match those in the shader code.
		// exposure_bias: Input scale (color *= bias, white *= bias) to make the brightness consistent with other tonemappers
		// also useful to scale the input to the range that the tonemapper is designed for (some require very high input values).
		// Has no effect on the curve's general shape or visual properties.
		const float exposure_bias = 2.0f;
		const float A = 0.22f * exposure_bias * exposure_bias; // bias baked into constants for performance
		const float B = 0.30f * exposure_bias;
		const float C = 0.10f;
		const float D = 0.20f;
		const float E = 0.01f;
		const float F = 0.30f;

		tonemap_parameters.white_tonemapped = ((white * (A * white + C * B) + D * E) / (white * (A * white + B) + D * F)) - E / F;
	} else if (env->tone_mapper == RSE::ENV_TONE_MAPPER_ACES) {
		// These constants must match those in the shader code.
		const float exposure_bias = 1.8f;
		const float A = 0.0245786f;
		const float B = 0.000090537f;
		const float C = 0.983729f;
		const float D = 0.432951f;
		const float E = 0.238081f;

		white *= exposure_bias;
		float white_tonemapped = (white * (white + A) - B) / (white * (C * white + D) + E);
		tonemap_parameters.white_tonemapped = white_tonemapped;
	} else if (env->tone_mapper == RSE::ENV_TONE_MAPPER_AGX) {
		// Calculate allenwp tonemapping curve parameters on the CPU to improve shader performance.
		// Source and details: https://allenwp.com/blog/2025/05/29/allenwp-tonemapping-curve/

		// These constants must match the those in the shader code.
		// 18% "middle gray" is perceptually 50% of the brightness of reference white.
		const float awp_crossover_point = 0.18;
		// When output_max_value and/or awp_crossover_point are no longer constant, awp_shoulder_max can
		// be calculated on the CPU and passed in as tonemap_parameters.tonemap_e.
		const float awp_shoulder_max = p_output_max_value - awp_crossover_point;

		float awp_high_clip = white;

		// awp_toe_a is a solution generated by Mathematica that ensures intersection at awp_crossover_point.
		float awp_toe_a = ((1.0 / awp_crossover_point) - 1.0) * pow(awp_crossover_point, env->tonemap_agx_contrast);
		// Slope formula is simply the derivative of the toe function with an input of awp_crossover_point.
		float awp_slope_denom = pow(awp_crossover_point, env->tonemap_agx_contrast) + awp_toe_a;
		float awp_slope = (env->tonemap_agx_contrast * pow(awp_crossover_point, env->tonemap_agx_contrast - 1.0) * awp_toe_a) / (awp_slope_denom * awp_slope_denom);

		float awp_w = awp_high_clip - awp_crossover_point;
		awp_w = awp_w * awp_w;
		awp_w = awp_w / awp_shoulder_max;
		awp_w = awp_w * awp_slope;

		tonemap_parameters.awp_contrast = env->tonemap_agx_contrast;
		tonemap_parameters.awp_toe_a = awp_toe_a;
		tonemap_parameters.awp_slope = awp_slope;
		tonemap_parameters.awp_w = awp_w;
	} else if (env->tone_mapper == RSE::ENV_TONE_MAPPER_GT7) {
		gt7_tonemap_parameters(p_output_max_value, tonemap_parameters.gt7_peak, tonemap_parameters.gt7_peak_intensity);
	}

	return tonemap_parameters;
}

void RendererEnvironmentStorage::gt7_tonemap_parameters(float p_output_max_value, float &r_peak, float &r_peak_intensity) {
	// GT7 places SDR paper white at 250 cd/m^2 (2.5 frame buffer units); the curve tunings assume a peak of at least
	// that (Polyphony Digital reference), the PQ range ends at 10000 cd/m^2. Must match tonemap_gt7_inc.glsl.
	const float paper_white = 2.5f;
	r_peak = CLAMP(paper_white * MAX(p_output_max_value, 1.0f), paper_white, 100.0f);

	// ICtCp intensity of grey at the peak: the PQ inverse EOTF of the peak (L = M = S for grey).
	const double m1 = 0.1593017578125;
	const double m2 = 78.84375;
	const double c1 = 0.8359375;
	const double c2 = 18.8515625;
	const double c3 = 18.6875;
	double ym = Math::pow(double(r_peak) * 100.0 / 10000.0, m1);
	r_peak_intensity = float(Math::pow((c1 + c2 * ym) / (1.0 + c3 * ym), m2));
}

// Fog

void RendererEnvironmentStorage::environment_set_fog(RID p_env, bool p_enable, const Color &p_light_color, float p_light_energy, float p_sun_scatter, float p_density, float p_height, float p_height_density, float p_fog_aerial_perspective, float p_sky_affect, RSE::EnvironmentFogMode p_mode) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->fog_enabled = p_enable;
	env->fog_mode = p_mode;
	env->fog_light_color = p_light_color;
	env->fog_light_energy = p_light_energy;
	env->fog_sun_scatter = p_sun_scatter;
	env->fog_density = p_density;
	env->fog_height = p_height;
	env->fog_height_density = p_height_density;
	env->fog_aerial_perspective = p_fog_aerial_perspective;
	env->fog_sky_affect = p_sky_affect;
}

bool RendererEnvironmentStorage::environment_get_fog_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->fog_enabled;
}

RSE::EnvironmentFogMode RendererEnvironmentStorage::environment_get_fog_mode(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::ENV_FOG_MODE_EXPONENTIAL);
	return env->fog_mode;
}

Color RendererEnvironmentStorage::environment_get_fog_light_color(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Color(0.5, 0.6, 0.7));
	return env->fog_light_color;
}

float RendererEnvironmentStorage::environment_get_fog_light_energy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->fog_light_energy;
}

float RendererEnvironmentStorage::environment_get_fog_sun_scatter(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_sun_scatter;
}

float RendererEnvironmentStorage::environment_get_fog_density(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.001);
	return env->fog_density;
}

float RendererEnvironmentStorage::environment_get_fog_height(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_height;
}

float RendererEnvironmentStorage::environment_get_fog_height_density(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_height_density;
}

float RendererEnvironmentStorage::environment_get_fog_aerial_perspective(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_aerial_perspective;
}

float RendererEnvironmentStorage::environment_get_fog_sky_affect(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_sky_affect;
}

// Depth Fog

void RendererEnvironmentStorage::environment_set_fog_depth(RID p_env, float p_curve, float p_begin, float p_end) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->fog_depth_curve = p_curve;
	env->fog_depth_begin = p_begin;
	env->fog_depth_end = p_end;
}

float RendererEnvironmentStorage::environment_get_fog_depth_curve(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_depth_curve;
}

float RendererEnvironmentStorage::environment_get_fog_depth_begin(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_depth_begin;
}

float RendererEnvironmentStorage::environment_get_fog_depth_end(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->fog_depth_end;
}

// Volumetric Fog

void RendererEnvironmentStorage::environment_set_volumetric_fog(RID p_env, bool p_enable, float p_density, const Color &p_albedo, const Color &p_emission, float p_emission_energy, float p_anisotropy, float p_length, float p_detail_spread, float p_gi_inject, bool p_temporal_reprojection, float p_temporal_reprojection_amount, float p_ambient_inject, float p_sky_affect) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_current_rendering_method() != "forward_plus" && p_enable) {
		WARN_PRINT_ONCE_ED("Volumetric fog is only available when using the Forward+ renderer.");
	}
#endif
	env->volumetric_fog_enabled = p_enable;
	env->volumetric_fog_density = p_density;
	env->volumetric_fog_scattering = p_albedo;
	env->volumetric_fog_emission = p_emission;
	env->volumetric_fog_emission_energy = p_emission_energy;
	env->volumetric_fog_anisotropy = p_anisotropy;
	env->volumetric_fog_length = p_length;
	env->volumetric_fog_detail_spread = p_detail_spread;
	env->volumetric_fog_gi_inject = p_gi_inject;
	env->volumetric_fog_temporal_reprojection = p_temporal_reprojection;
	env->volumetric_fog_temporal_reprojection_amount = p_temporal_reprojection_amount;
	env->volumetric_fog_ambient_inject = p_ambient_inject;
	env->volumetric_fog_sky_affect = p_sky_affect;
}

bool RendererEnvironmentStorage::environment_get_volumetric_fog_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->volumetric_fog_enabled;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_density(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.01);
	return env->volumetric_fog_density;
}

Color RendererEnvironmentStorage::environment_get_volumetric_fog_scattering(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Color(1, 1, 1));
	return env->volumetric_fog_scattering;
}

Color RendererEnvironmentStorage::environment_get_volumetric_fog_emission(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Color(0, 0, 0));
	return env->volumetric_fog_emission;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_emission_energy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->volumetric_fog_emission_energy;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_anisotropy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.2);
	return env->volumetric_fog_anisotropy;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_length(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 64.0);
	return env->volumetric_fog_length;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_detail_spread(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 2.0);
	return env->volumetric_fog_detail_spread;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_gi_inject(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->volumetric_fog_gi_inject;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_sky_affect(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->volumetric_fog_sky_affect;
}

bool RendererEnvironmentStorage::environment_get_volumetric_fog_temporal_reprojection(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, true);
	return env->volumetric_fog_temporal_reprojection;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_temporal_reprojection_amount(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.9);
	return env->volumetric_fog_temporal_reprojection_amount;
}

float RendererEnvironmentStorage::environment_get_volumetric_fog_ambient_inject(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->volumetric_fog_ambient_inject;
}

// GLOW

void RendererEnvironmentStorage::environment_set_glow(RID p_env, bool p_enable, Vector<float> p_levels, float p_intensity, float p_strength, float p_mix, float p_bloom_threshold, RSE::EnvironmentGlowBlendMode p_blend_mode, float p_hdr_bleed_threshold, float p_hdr_bleed_scale, float p_hdr_luminance_cap, float p_glow_map_strength, RID p_glow_map) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	ERR_FAIL_COND_MSG(p_levels.size() != 7, "Size of array of glow levels must be 7");
	env->glow_enabled = p_enable;
	env->glow_levels = p_levels;
	env->glow_intensity = p_intensity;
	env->glow_strength = p_strength;
	env->glow_mix = p_mix;
	env->glow_bloom = p_bloom_threshold;
	env->glow_blend_mode = p_blend_mode;
	env->glow_hdr_bleed_threshold = p_hdr_bleed_threshold;
	env->glow_hdr_bleed_scale = p_hdr_bleed_scale;
	env->glow_hdr_luminance_cap = p_hdr_luminance_cap;
	env->glow_map_strength = p_glow_map_strength;
	env->glow_map = p_glow_map;
}

bool RendererEnvironmentStorage::environment_get_glow_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->glow_enabled;
}

Vector<float> RendererEnvironmentStorage::environment_get_glow_levels(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Vector<float>());
	return env->glow_levels;
}

float RendererEnvironmentStorage::environment_get_glow_intensity(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.3);
	return env->glow_intensity;
}

float RendererEnvironmentStorage::environment_get_glow_strength(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->glow_strength;
}

float RendererEnvironmentStorage::environment_get_glow_bloom(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->glow_bloom;
}

float RendererEnvironmentStorage::environment_get_glow_mix(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.01);
	return env->glow_mix;
}

RSE::EnvironmentGlowBlendMode RendererEnvironmentStorage::environment_get_glow_blend_mode(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::ENV_GLOW_BLEND_MODE_SCREEN);
	return env->glow_blend_mode;
}

float RendererEnvironmentStorage::environment_get_glow_hdr_bleed_threshold(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->glow_hdr_bleed_threshold;
}

float RendererEnvironmentStorage::environment_get_glow_hdr_luminance_cap(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 12.0);
	return env->glow_hdr_luminance_cap;
}

float RendererEnvironmentStorage::environment_get_glow_hdr_bleed_scale(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 2.0);
	return env->glow_hdr_bleed_scale;
}

float RendererEnvironmentStorage::environment_get_glow_map_strength(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->glow_map_strength;
}

RID RendererEnvironmentStorage::environment_get_glow_map(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RID());
	return env->glow_map;
}

// SSR

void RendererEnvironmentStorage::environment_set_ssr(RID p_env, bool p_enable, int p_max_steps, float p_fade_int, float p_fade_out, float p_depth_tolerance) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_current_rendering_method() != "forward_plus" && p_enable) {
		WARN_PRINT_ONCE_ED("Screen-space reflections (SSR) are only available when using the Forward+ renderer.");
	}
#endif
	env->ssr_enabled = p_enable;
	env->ssr_max_steps = p_max_steps;
	env->ssr_fade_in = p_fade_int;
	env->ssr_fade_out = p_fade_out;
	env->ssr_depth_tolerance = p_depth_tolerance;
}

bool RendererEnvironmentStorage::environment_get_ssr_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->ssr_enabled;
}

int RendererEnvironmentStorage::environment_get_ssr_max_steps(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 64);
	return env->ssr_max_steps;
}

float RendererEnvironmentStorage::environment_get_ssr_fade_in(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.15);
	return env->ssr_fade_in;
}

float RendererEnvironmentStorage::environment_get_ssr_fade_out(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 2.0);
	return env->ssr_fade_out;
}

float RendererEnvironmentStorage::environment_get_ssr_depth_tolerance(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.5);
	return env->ssr_depth_tolerance;
}

// SSAO

void RendererEnvironmentStorage::environment_set_ssao(RID p_env, bool p_enable, float p_radius, float p_intensity, float p_power, float p_detail, float p_horizon, float p_sharpness, float p_light_affect, float p_ao_channel_affect) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_current_rendering_method() == "mobile" && p_enable) {
		WARN_PRINT_ONCE_ED("Screen-space ambient occlusion (SSAO) is only available when using the Forward+ or Compatibility renderers.");
	}
#endif
	env->ssao_enabled = p_enable;
	env->ssao_radius = p_radius;
	env->ssao_intensity = p_intensity;
	env->ssao_power = p_power;
	env->ssao_detail = p_detail;
	env->ssao_horizon = p_horizon;
	env->ssao_sharpness = p_sharpness;
	env->ssao_direct_light_affect = p_light_affect;
	env->ssao_ao_channel_affect = p_ao_channel_affect;
}

bool RendererEnvironmentStorage::environment_get_ssao_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->ssao_enabled;
}

float RendererEnvironmentStorage::environment_get_ssao_radius(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ssao_radius;
}

float RendererEnvironmentStorage::environment_get_ssao_intensity(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 2.0);
	return env->ssao_intensity;
}

float RendererEnvironmentStorage::environment_get_ssao_power(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.5);
	return env->ssao_power;
}

float RendererEnvironmentStorage::environment_get_ssao_detail(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.5);
	return env->ssao_detail;
}

float RendererEnvironmentStorage::environment_get_ssao_horizon(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.06);
	return env->ssao_horizon;
}

float RendererEnvironmentStorage::environment_get_ssao_sharpness(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.98);
	return env->ssao_sharpness;
}

float RendererEnvironmentStorage::environment_get_ssao_direct_light_affect(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->ssao_direct_light_affect;
}

float RendererEnvironmentStorage::environment_get_ssao_ao_channel_affect(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->ssao_ao_channel_affect;
}

// SSIL

void RendererEnvironmentStorage::environment_set_ssil(RID p_env, bool p_enable, float p_radius, float p_intensity, float p_sharpness, float p_normal_rejection) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_current_rendering_method() != "forward_plus" && p_enable) {
		WARN_PRINT_ONCE_ED("Screen-space indirect lighting (SSIL) is only available when using the Forward+ renderer.");
	}
#endif
	env->ssil_enabled = p_enable;
	env->ssil_radius = p_radius;
	env->ssil_intensity = p_intensity;
	env->ssil_sharpness = p_sharpness;
	env->ssil_normal_rejection = p_normal_rejection;
}

bool RendererEnvironmentStorage::environment_get_ssil_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->ssil_enabled;
}

float RendererEnvironmentStorage::environment_get_ssil_radius(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 5.0);
	return env->ssil_radius;
}

float RendererEnvironmentStorage::environment_get_ssil_intensity(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ssil_intensity;
}

float RendererEnvironmentStorage::environment_get_ssil_sharpness(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.98);
	return env->ssil_sharpness;
}

float RendererEnvironmentStorage::environment_get_ssil_normal_rejection(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ssil_normal_rejection;
}

// SDFGI

void RendererEnvironmentStorage::environment_set_sdfgi(RID p_env, bool p_enable, int p_cascades, float p_min_cell_size, RSE::EnvironmentSDFGIYScale p_y_scale, bool p_use_occlusion, float p_bounce_feedback, bool p_read_sky, float p_energy, float p_normal_bias, float p_probe_bias) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_current_rendering_method() != "forward_plus" && p_enable) {
		WARN_PRINT_ONCE_ED("SDFGI is only available when using the Forward+ renderer.");
	}
#endif
	env->sdfgi_enabled = p_enable;
	env->sdfgi_cascades = p_cascades;
	env->sdfgi_min_cell_size = p_min_cell_size;
	env->sdfgi_use_occlusion = p_use_occlusion;
	env->sdfgi_bounce_feedback = p_bounce_feedback;
	env->sdfgi_read_sky_light = p_read_sky;
	env->sdfgi_energy = p_energy;
	env->sdfgi_normal_bias = p_normal_bias;
	env->sdfgi_probe_bias = p_probe_bias;
	env->sdfgi_y_scale = p_y_scale;
}

bool RendererEnvironmentStorage::environment_get_sdfgi_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->sdfgi_enabled;
}

int RendererEnvironmentStorage::environment_get_sdfgi_cascades(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 4);
	return env->sdfgi_cascades;
}

float RendererEnvironmentStorage::environment_get_sdfgi_min_cell_size(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.2);
	return env->sdfgi_min_cell_size;
}

bool RendererEnvironmentStorage::environment_get_sdfgi_use_occlusion(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->sdfgi_use_occlusion;
}

float RendererEnvironmentStorage::environment_get_sdfgi_bounce_feedback(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.5);
	return env->sdfgi_bounce_feedback;
}

bool RendererEnvironmentStorage::environment_get_sdfgi_read_sky_light(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, true);
	return env->sdfgi_read_sky_light;
}

float RendererEnvironmentStorage::environment_get_sdfgi_energy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->sdfgi_energy;
}

float RendererEnvironmentStorage::environment_get_sdfgi_normal_bias(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.1);
	return env->sdfgi_normal_bias;
}

float RendererEnvironmentStorage::environment_get_sdfgi_probe_bias(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.1);
	return env->sdfgi_probe_bias;
}

RSE::EnvironmentSDFGIYScale RendererEnvironmentStorage::environment_get_sdfgi_y_scale(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::ENV_SDFGI_Y_SCALE_75_PERCENT);
	return env->sdfgi_y_scale;
}

// Adjustments

void RendererEnvironmentStorage::environment_set_adjustment(RID p_env, bool p_enable, float p_brightness, float p_contrast, float p_saturation, bool p_use_1d_color_correction, RID p_color_correction) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);

	env->adjustments_enabled = p_enable;
	// Scale brightness via the nonlinear sRGB transfer function to provide a
	// somewhat perceptually uniform brightness adjustment.
	env->adjustments_brightness = p_brightness < 0.04045f ? p_brightness * (1.0f / 12.92f) : Math::pow(float((p_brightness + 0.055f) * (1.0f / (1.055f))), 2.4f);
	env->adjustments_contrast = p_contrast;
	env->adjustments_saturation = p_saturation;
	env->use_1d_color_correction = p_use_1d_color_correction;
	env->color_correction = p_color_correction;
}

bool RendererEnvironmentStorage::environment_get_adjustments_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->adjustments_enabled;
}

float RendererEnvironmentStorage::environment_get_adjustments_brightness(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->adjustments_brightness;
}

float RendererEnvironmentStorage::environment_get_adjustments_contrast(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->adjustments_contrast;
}

float RendererEnvironmentStorage::environment_get_adjustments_saturation(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->adjustments_saturation;
}

bool RendererEnvironmentStorage::environment_get_use_1d_color_correction(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->use_1d_color_correction;
}

RID RendererEnvironmentStorage::environment_get_color_correction(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RID());
	return env->color_correction;
}

// Path tracing (Mariner E5c)

void RendererEnvironmentStorage::environment_set_pathtracing(RID p_env, bool p_enable, int p_debug_mode, int p_samples_per_pixel, int p_max_bounces, RSE::PathtracingDenoiser p_denoiser) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->pathtracing_enabled = p_enable;
	env->pathtracing_debug_mode = CLAMP(p_debug_mode, 0, 22);
	env->pathtracing_samples_per_pixel = CLAMP(p_samples_per_pixel, 1, 64);
	env->pathtracing_max_bounces = CLAMP(p_max_bounces, 1, 8);
	env->pathtracing_denoiser = p_denoiser;
}

void RendererEnvironmentStorage::environment_set_pathtracing_accumulation(RID p_env, bool p_accumulate, int p_max_samples, float p_firefly_clamp) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->pathtracing_accumulate = p_accumulate;
	env->pathtracing_max_samples = MAX(p_max_samples, 0);
	env->pathtracing_firefly_clamp = MAX(p_firefly_clamp, 0.0f);
}

bool RendererEnvironmentStorage::environment_get_pathtracing_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->pathtracing_enabled;
}

int RendererEnvironmentStorage::environment_get_pathtracing_debug_mode(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0);
	return env->pathtracing_debug_mode;
}

int RendererEnvironmentStorage::environment_get_pathtracing_samples_per_pixel(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1);
	return env->pathtracing_samples_per_pixel;
}

int RendererEnvironmentStorage::environment_get_pathtracing_max_bounces(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 3);
	return env->pathtracing_max_bounces;
}

RSE::PathtracingDenoiser RendererEnvironmentStorage::environment_get_pathtracing_denoiser(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, RSE::PT_DENOISER_NONE);
	return env->pathtracing_denoiser;
}

bool RendererEnvironmentStorage::environment_get_pathtracing_accumulate(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, true);
	return env->pathtracing_accumulate;
}

int RendererEnvironmentStorage::environment_get_pathtracing_max_samples(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0);
	return env->pathtracing_max_samples;
}

float RendererEnvironmentStorage::environment_get_pathtracing_firefly_clamp(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->pathtracing_firefly_clamp;
}

// Dynamic diffuse GI probes (Mariner E12)

Vector3i RendererEnvironmentStorage::ddgi_clamp_probe_count(const Vector3i &p_count) {
	// Atlas limits: probes of one Z row side by side (x * y tiles of 18 texels, at most 16384), at most 16384 probes.
	Vector3i c(CLAMP(p_count.x, 2, 64), CLAMP(p_count.y, 2, 32), CLAMP(p_count.z, 2, 64));
	while (c.x * c.y > 896 && c.y > 2) {
		c.y--;
	}
	while (c.x * c.y * c.z > 16384 && c.z > 2) {
		c.z--;
	}
	return c;
}

Vector3i RendererEnvironmentStorage::ddgi_volume_probe_count(const Vector3 &p_size, float p_spacing) {
	const float spacing = MAX(p_spacing, 0.01f);
	const Vector3 s = p_size.maxf(0.0);
	return ddgi_clamp_probe_count(Vector3i(int(Math::round(s.x / spacing)) + 1, int(Math::round(s.y / spacing)) + 1, int(Math::round(s.z / spacing)) + 1));
}

void RendererEnvironmentStorage::environment_set_ddgi(RID p_env, bool p_enable, const Vector3i &p_probe_count, float p_probe_spacing, int p_rays_per_probe, float p_hysteresis, float p_energy, float p_bias, float p_specular_occlusion) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->ddgi_enabled = p_enable;
	env->ddgi_probe_count = ddgi_clamp_probe_count(p_probe_count);
	env->ddgi_probe_spacing = CLAMP(p_probe_spacing, 0.1f, 64.0f);
	env->ddgi_rays_per_probe = CLAMP((p_rays_per_probe + 31) / 32 * 32, 32, 256);
	env->ddgi_hysteresis = CLAMP(p_hysteresis, 0.0f, 0.995f);
	env->ddgi_energy = MAX(p_energy, 0.0f);
	env->ddgi_bias = MAX(p_bias, 0.0f);
	env->ddgi_specular_occlusion = CLAMP(p_specular_occlusion, 0.0f, 1.0f);
}

bool RendererEnvironmentStorage::environment_get_ddgi_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->ddgi_enabled;
}

Vector3i RendererEnvironmentStorage::environment_get_ddgi_probe_count(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Vector3i(24, 8, 24));
	return env->ddgi_probe_count;
}

float RendererEnvironmentStorage::environment_get_ddgi_probe_spacing(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 2.0);
	return env->ddgi_probe_spacing;
}

int RendererEnvironmentStorage::environment_get_ddgi_rays_per_probe(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 128);
	return env->ddgi_rays_per_probe;
}

float RendererEnvironmentStorage::environment_get_ddgi_hysteresis(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.97);
	return env->ddgi_hysteresis;
}

float RendererEnvironmentStorage::environment_get_ddgi_energy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ddgi_energy;
}

float RendererEnvironmentStorage::environment_get_ddgi_bias(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.3);
	return env->ddgi_bias;
}

float RendererEnvironmentStorage::environment_get_ddgi_specular_occlusion(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ddgi_specular_occlusion;
}

void RendererEnvironmentStorage::environment_set_ddgi_update(RID p_env, int p_probes_per_frame, bool p_relocation) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->ddgi_probes_per_frame = MAX(p_probes_per_frame, 0);
	env->ddgi_relocation = p_relocation;
}

int RendererEnvironmentStorage::environment_get_ddgi_probes_per_frame(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0);
	return env->ddgi_probes_per_frame;
}

bool RendererEnvironmentStorage::environment_get_ddgi_relocation(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, true);
	return env->ddgi_relocation;
}

void RendererEnvironmentStorage::environment_set_ddgi_channels(RID p_env, float p_sky_energy, float p_lights_energy) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->ddgi_sky_energy = MAX(p_sky_energy, 0.0f);
	env->ddgi_lights_energy = MAX(p_lights_energy, 0.0f);
}

float RendererEnvironmentStorage::environment_get_ddgi_sky_energy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ddgi_sky_energy;
}

float RendererEnvironmentStorage::environment_get_ddgi_lights_energy(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 1.0);
	return env->ddgi_lights_energy;
}

void RendererEnvironmentStorage::environment_set_ddgi_sea(RID p_env, bool p_enable, float p_level, const Color &p_color) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->ddgi_sea = p_enable;
	env->ddgi_sea_level = p_level;
	env->ddgi_sea_color = p_color;
}

bool RendererEnvironmentStorage::environment_get_ddgi_sea(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->ddgi_sea;
}

float RendererEnvironmentStorage::environment_get_ddgi_sea_level(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0);
	return env->ddgi_sea_level;
}

Color RendererEnvironmentStorage::environment_get_ddgi_sea_color(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, Color());
	return env->ddgi_sea_color;
}

// Mariner E18.
void RendererEnvironmentStorage::environment_set_ssao_gtao(RID p_env, bool p_enable, bool p_bent_normals, float p_thickness) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->ssao_gtao = p_enable;
	env->ssao_bent_normals = p_bent_normals;
	env->ssao_gtao_thickness = MAX(p_thickness, 0.0f);
}

void RendererEnvironmentStorage::environment_set_restir(RID p_env, bool p_enable, int p_candidates, int p_spatial_samples, bool p_temporal, bool p_denoise) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->restir_enabled = p_enable;
	env->restir_candidates = CLAMP(p_candidates, 1, 32);
	env->restir_spatial_samples = CLAMP(p_spatial_samples, 0, 8);
	env->restir_temporal = p_temporal;
	env->restir_denoise = p_denoise;
}

bool RendererEnvironmentStorage::environment_get_restir_enabled(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->restir_enabled;
}

int RendererEnvironmentStorage::environment_get_restir_candidates(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 8);
	return env->restir_candidates;
}

int RendererEnvironmentStorage::environment_get_restir_spatial_samples(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 3);
	return env->restir_spatial_samples;
}

bool RendererEnvironmentStorage::environment_get_restir_temporal(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, true);
	return env->restir_temporal;
}

bool RendererEnvironmentStorage::environment_get_restir_denoise(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, true);
	return env->restir_denoise;
}

bool RendererEnvironmentStorage::environment_get_ssao_gtao(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, false);
	return env->ssao_gtao;
}

bool RendererEnvironmentStorage::environment_get_ssao_bent_normals(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, true);
	return env->ssao_bent_normals;
}

// Mariner F3.
void RendererEnvironmentStorage::environment_set_color_grading(RID p_env, bool p_enable, float p_temperature, float p_tint, const Color &p_shadows, const Color &p_midtones, const Color &p_highlights, const Vector4 &p_tonal_ranges, const Vector3 &p_slope, const Vector3 &p_offset, const Vector3 &p_power, float p_saturation, bool p_gamut_mapping) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	ColorGrading &cg = env->color_grading;
	cg.enabled = p_enable;
	cg.temperature = CLAMP(p_temperature, 1667.0f, 25000.0f);
	cg.tint = CLAMP(p_tint, -1.0f, 1.0f);
	cg.shadows = p_shadows;
	cg.midtones = p_midtones;
	cg.highlights = p_highlights;
	cg.tonal_ranges = p_tonal_ranges;
	cg.slope = p_slope.maxf(0.0);
	cg.offset = p_offset;
	cg.power = p_power.maxf(0.01);
	cg.saturation = MAX(p_saturation, 0.0f);
	cg.gamut_mapping = p_gamut_mapping;
}

// Mariner F3b.
void RendererEnvironmentStorage::environment_set_color_grading_night_vision(RID p_env, float p_amount, const Color &p_tint, float p_start, float p_end) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	ColorGrading &cg = env->color_grading;
	cg.night_vision = CLAMP(p_amount, 0.0f, 1.0f);
	cg.night_vision_tint = p_tint;
	cg.night_vision_start = MAX(p_start, 1e-8f);
	cg.night_vision_end = MAX(p_end, 1e-9f);
}

// Mariner F3c.
void RendererEnvironmentStorage::environment_set_color_grading_channel_mixer(RID p_env, const Vector3 &p_red, const Vector3 &p_green, const Vector3 &p_blue) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	ColorGrading &cg = env->color_grading;
	cg.mixer_red = p_red;
	cg.mixer_green = p_green;
	cg.mixer_blue = p_blue;
}

void RendererEnvironmentStorage::environment_set_color_grading_curves(RID p_env, float p_contrast, float p_vibrance, const Vector3 &p_shadow_gamma, const Vector3 &p_mid_point, const Vector3 &p_highlight_scale) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	ColorGrading &cg = env->color_grading;
	cg.contrast = MAX(p_contrast, 0.0f);
	cg.vibrance = MAX(p_vibrance, 0.0f);
	cg.curves_shadow_gamma = p_shadow_gamma.maxf(0.01);
	cg.curves_mid_point = p_mid_point.maxf(1e-4);
	cg.curves_highlight_scale = p_highlight_scale.maxf(0.0);
}

RendererEnvironmentStorage::ColorGrading RendererEnvironmentStorage::environment_get_color_grading(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, ColorGrading());
	return env->color_grading;
}

// Mariner F1.
float RendererEnvironmentStorage::environment_get_ssao_gtao_thickness(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.5);
	return env->ssao_gtao_thickness;
}

// Overhead map (Mariner G1a).

void RendererEnvironmentStorage::overhead_map_region(const Vector3 &p_camera, float p_size, float p_height, int p_resolution, Vector3 &r_min, Vector3 &r_max) {
	const float texel = p_size / float(MAX(p_resolution, 1));
	const float cx = Math::round(p_camera.x / texel) * texel;
	const float cz = Math::round(p_camera.z / texel) * texel;
	const float top = Math::ceil(p_camera.y + p_height * 0.5f);
	r_min = Vector3(cx - p_size * 0.5f, top - p_height, cz - p_size * 0.5f);
	r_max = Vector3(cx + p_size * 0.5f, top, cz + p_size * 0.5f);
}

void RendererEnvironmentStorage::environment_set_overhead_map(RID p_env, bool p_enable, float p_size, float p_height, int p_resolution, uint32_t p_cull_mask, float p_sky_occlusion, float p_sky_occlusion_radius, float p_bias) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	OverheadMapSettings &o = env->overhead_map;
	o.enabled = p_enable;
	o.size = CLAMP(p_size, 16.0f, 8192.0f);
	o.height = CLAMP(p_height, 4.0f, 4096.0f);
	o.resolution = CLAMP(int(Math::next_power_of_2(uint32_t(MAX(p_resolution, 1)))), 128, 4096);
	o.cull_mask = p_cull_mask;
	o.sky_occlusion = CLAMP(p_sky_occlusion, 0.0f, 1.0f);
	o.sky_occlusion_radius = CLAMP(p_sky_occlusion_radius, 0.5f, o.size * 0.5f);
	o.bias = CLAMP(p_bias, 0.0f, 16.0f);
}

RendererEnvironmentStorage::OverheadMapSettings RendererEnvironmentStorage::environment_get_overhead_map(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, OverheadMapSettings());
	return env->overhead_map;
}

// Wetness of the world (Mariner G4).

void RendererEnvironmentStorage::environment_set_wetness(RID p_env, float p_wetness) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->wetness = CLAMP(p_wetness, 0.0f, 1.0f);
}

float RendererEnvironmentStorage::environment_get_wetness(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0f);
	return env->wetness;
}

// Snow on the world (Mariner G4b).

void RendererEnvironmentStorage::environment_set_snow(RID p_env, float p_snow) {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL(env);
	env->snow = CLAMP(p_snow, 0.0f, 1.0f);
}

float RendererEnvironmentStorage::environment_get_snow(RID p_env) const {
	Environment *env = environment_owner.get_or_null(p_env);
	ERR_FAIL_NULL_V(env, 0.0f);
	return env->snow;
}
