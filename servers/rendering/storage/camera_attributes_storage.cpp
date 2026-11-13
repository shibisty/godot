/**************************************************************************/
/*  camera_attributes_storage.cpp                                         */
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

#include "camera_attributes_storage.h"

#ifdef DEBUG_ENABLED
#include "core/os/os.h"
#endif

RendererCameraAttributes *RendererCameraAttributes::singleton = nullptr;
uint64_t RendererCameraAttributes::auto_exposure_counter = 2;

RendererCameraAttributes::RendererCameraAttributes() {
	singleton = this;
}

RendererCameraAttributes::~RendererCameraAttributes() {
	singleton = nullptr;
}

RID RendererCameraAttributes::camera_attributes_allocate() {
	return camera_attributes_owner.allocate_rid();
}

void RendererCameraAttributes::camera_attributes_initialize(RID p_rid) {
	camera_attributes_owner.initialize_rid(p_rid, CameraAttributes());
}

void RendererCameraAttributes::camera_attributes_free(RID p_rid) {
	camera_attributes_owner.free(p_rid);
}

void RendererCameraAttributes::camera_attributes_set_dof_blur_quality(RSE::DOFBlurQuality p_quality, bool p_use_jitter) {
	dof_blur_quality = p_quality;
	dof_blur_use_jitter = p_use_jitter;
}

void RendererCameraAttributes::camera_attributes_set_dof_blur_bokeh_shape(RSE::DOFBokehShape p_shape) {
	dof_blur_bokeh_shape = p_shape;
}

void RendererCameraAttributes::camera_attributes_set_dof_blur(RID p_camera_attributes, bool p_far_enable, float p_far_distance, float p_far_transition, bool p_near_enable, float p_near_distance, float p_near_transition, float p_amount) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
#ifdef DEBUG_ENABLED
	if (OS::get_singleton()->get_current_rendering_method() == "gl_compatibility" && (p_far_enable || p_near_enable)) {
		WARN_PRINT_ONCE_ED("Depth of field blur is only available when using the Forward+ or Mobile renderer.");
	}
#endif
	cam_attributes->dof_blur_far_enabled = p_far_enable;
	cam_attributes->dof_blur_far_distance = p_far_distance;
	cam_attributes->dof_blur_far_transition = p_far_transition;

	cam_attributes->dof_blur_near_enabled = p_near_enable;
	cam_attributes->dof_blur_near_distance = p_near_distance;
	cam_attributes->dof_blur_near_transition = p_near_transition;

	cam_attributes->dof_blur_amount = p_amount;
}

void RendererCameraAttributes::camera_attributes_set_motion_blur(RID p_camera_attributes, bool p_enable, float p_intensity, float p_camera_motion, float p_max_length) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
	cam_attributes->motion_blur_enabled = p_enable;
	cam_attributes->motion_blur_intensity = CLAMP(p_intensity, 0.0f, 4.0f);
	cam_attributes->motion_blur_camera_motion = CLAMP(p_camera_motion, 0.0f, 1.0f);
	cam_attributes->motion_blur_max_length = CLAMP(p_max_length, 0.0f, 0.25f);
}

float RendererCameraAttributes::camera_attributes_get_motion_blur_intensity(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->motion_blur_intensity;
}

float RendererCameraAttributes::camera_attributes_get_motion_blur_camera_motion(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 1.0);
	return cam_attributes->motion_blur_camera_motion;
}

float RendererCameraAttributes::camera_attributes_get_motion_blur_max_length(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->motion_blur_max_length;
}

void RendererCameraAttributes::camera_attributes_set_lens_flare(RID p_camera_attributes, bool p_enable, float p_intensity, float p_threshold, int p_ghosts, float p_ghost_spacing, float p_halo_width, float p_chromatic, float p_sun_streaks, int p_streak_count) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
	cam_attributes->lens_flare_enabled = p_enable;
	cam_attributes->lens_flare_intensity = MAX(p_intensity, 0.0f);
	cam_attributes->lens_flare_threshold = MAX(p_threshold, 0.0f);
	cam_attributes->lens_flare_ghosts = CLAMP(p_ghosts, 0, 8);
	cam_attributes->lens_flare_ghost_spacing = CLAMP(p_ghost_spacing, 0.0f, 1.0f);
	cam_attributes->lens_flare_halo_width = CLAMP(p_halo_width, 0.0f, 1.0f);
	cam_attributes->lens_flare_chromatic = CLAMP(p_chromatic, 0.0f, 0.1f);
	cam_attributes->lens_flare_sun_streaks = MAX(p_sun_streaks, 0.0f);
	cam_attributes->lens_flare_streak_count = CLAMP(p_streak_count, 2, 16);
}

void RendererCameraAttributes::camera_attributes_set_lens_dirt(RID p_camera_attributes, RID p_texture, float p_intensity) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
	cam_attributes->lens_dirt_texture = p_texture;
	cam_attributes->lens_dirt_intensity = MAX(p_intensity, 0.0f);
}

bool RendererCameraAttributes::camera_attributes_get_lens_flare(RID p_camera_attributes, LensFlare &r_lens_flare) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	if (!cam_attributes || !cam_attributes->lens_flare_enabled) {
		return false;
	}
	r_lens_flare.intensity = cam_attributes->lens_flare_intensity;
	r_lens_flare.threshold = cam_attributes->lens_flare_threshold;
	r_lens_flare.ghosts = cam_attributes->lens_flare_ghosts;
	r_lens_flare.ghost_spacing = cam_attributes->lens_flare_ghost_spacing;
	r_lens_flare.halo_width = cam_attributes->lens_flare_halo_width;
	r_lens_flare.chromatic = cam_attributes->lens_flare_chromatic;
	r_lens_flare.sun_streaks = cam_attributes->lens_flare_sun_streaks;
	r_lens_flare.streak_count = cam_attributes->lens_flare_streak_count;
	r_lens_flare.dirt_texture = cam_attributes->lens_dirt_texture;
	r_lens_flare.dirt_intensity = cam_attributes->lens_dirt_intensity;
	return r_lens_flare.intensity > 0.0f || r_lens_flare.sun_streaks > 0.0f;
}

void RendererCameraAttributes::camera_attributes_set_image_effects(RID p_camera_attributes, float p_chromatic_aberration, float p_lens_distortion, float p_film_grain, float p_film_grain_size, float p_sharpening) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
	cam_attributes->chromatic_aberration = CLAMP(p_chromatic_aberration, 0.0f, 1.0f);
	cam_attributes->lens_distortion = CLAMP(p_lens_distortion, -1.0f, 1.0f);
	cam_attributes->film_grain = CLAMP(p_film_grain, 0.0f, 1.0f);
	cam_attributes->film_grain_size = CLAMP(p_film_grain_size, 1.0f, 3.99f);
	cam_attributes->sharpening = CLAMP(p_sharpening, 0.0f, 1.0f);
}

RendererCameraAttributes::ImageEffects RendererCameraAttributes::camera_attributes_get_image_effects(RID p_camera_attributes) const {
	ImageEffects effects;
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	if (cam_attributes) {
		effects.chromatic_aberration = cam_attributes->chromatic_aberration;
		effects.lens_distortion = cam_attributes->lens_distortion;
		effects.film_grain = cam_attributes->film_grain;
		effects.film_grain_size = cam_attributes->film_grain_size;
		effects.sharpening = cam_attributes->sharpening;
	}
	return effects;
}

bool RendererCameraAttributes::camera_attributes_get_dof_far_enabled(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, false);
	return cam_attributes->dof_blur_far_enabled;
}

float RendererCameraAttributes::camera_attributes_get_dof_far_distance(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->dof_blur_far_distance;
}

float RendererCameraAttributes::camera_attributes_get_dof_far_transition(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->dof_blur_far_transition;
}

bool RendererCameraAttributes::camera_attributes_get_dof_near_enabled(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, false);
	return cam_attributes->dof_blur_near_enabled;
}

float RendererCameraAttributes::camera_attributes_get_dof_near_distance(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->dof_blur_near_distance;
}

float RendererCameraAttributes::camera_attributes_get_dof_near_transition(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->dof_blur_near_transition;
}

float RendererCameraAttributes::camera_attributes_get_dof_blur_amount(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->dof_blur_amount;
}

void RendererCameraAttributes::camera_attributes_set_exposure(RID p_camera_attributes, float p_multiplier, float p_exposure_normalization) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
	cam_attributes->exposure_multiplier = p_multiplier;
	cam_attributes->exposure_normalization = p_exposure_normalization;
}

float RendererCameraAttributes::camera_attributes_get_exposure_normalization_factor(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 1.0);

	return cam_attributes->exposure_multiplier * cam_attributes->exposure_normalization;
}

void RendererCameraAttributes::camera_attributes_set_auto_exposure(RID p_camera_attributes, bool p_enable, float p_min_sensitivity, float p_max_sensitivity, float p_speed, float p_scale) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
	if (!cam_attributes->use_auto_exposure && p_enable) {
		cam_attributes->auto_exposure_version = ++auto_exposure_counter;
	}
#ifdef DEBUG_ENABLED
	if ((OS::get_singleton()->get_current_rendering_method() == "gl_compatibility" || OS::get_singleton()->get_current_rendering_method() == "mobile") && p_enable) {
		WARN_PRINT_ONCE_ED("Auto-exposure is only available when using the Forward+ renderer.");
	}
#endif
	cam_attributes->use_auto_exposure = p_enable;
	cam_attributes->auto_exposure_min_sensitivity = p_min_sensitivity;
	cam_attributes->auto_exposure_max_sensitivity = p_max_sensitivity;
	cam_attributes->auto_exposure_adjust_speed = p_speed;
	cam_attributes->auto_exposure_scale = p_scale;
}

float RendererCameraAttributes::camera_attributes_get_auto_exposure_min_sensitivity(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->auto_exposure_min_sensitivity;
}

float RendererCameraAttributes::camera_attributes_get_auto_exposure_max_sensitivity(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->auto_exposure_max_sensitivity;
}

float RendererCameraAttributes::camera_attributes_get_auto_exposure_adjust_speed(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->auto_exposure_adjust_speed;
}

float RendererCameraAttributes::camera_attributes_get_auto_exposure_scale(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0.0);
	return cam_attributes->auto_exposure_scale;
}

// Mariner G2.
void RendererCameraAttributes::camera_attributes_set_auto_exposure_histogram(RID p_camera_attributes, bool p_histogram, float p_low_percentile, float p_high_percentile, float p_center_weight, float p_speed_dark) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL(cam_attributes);
	cam_attributes->auto_exposure_histogram = p_histogram;
	cam_attributes->auto_exposure_low_percentile = CLAMP(p_low_percentile, 0.0f, 0.99f);
	cam_attributes->auto_exposure_high_percentile = CLAMP(p_high_percentile, cam_attributes->auto_exposure_low_percentile + 0.01f, 1.0f);
	cam_attributes->auto_exposure_center_weight = CLAMP(p_center_weight, 0.0f, 1.0f);
	cam_attributes->auto_exposure_speed_dark = MAX(p_speed_dark, 0.0f);
}

RendererCameraAttributes::AutoExposureHistogram RendererCameraAttributes::camera_attributes_get_auto_exposure_histogram(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	AutoExposureHistogram r;
	ERR_FAIL_NULL_V(cam_attributes, r);
	r.enabled = cam_attributes->auto_exposure_histogram;
	r.low_percentile = cam_attributes->auto_exposure_low_percentile;
	r.high_percentile = cam_attributes->auto_exposure_high_percentile;
	r.center_weight = cam_attributes->auto_exposure_center_weight;
	r.speed_dark = cam_attributes->auto_exposure_speed_dark;
	return r;
}

uint64_t RendererCameraAttributes::camera_attributes_get_auto_exposure_version(RID p_camera_attributes) {
	CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
	ERR_FAIL_NULL_V(cam_attributes, 0);
	return cam_attributes->auto_exposure_version;
}
