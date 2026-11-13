/**************************************************************************/
/*  camera_attributes_storage.h                                           */
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

#include "core/templates/rid_owner.h"
#include "servers/rendering/rendering_server_enums.h"

class RendererCameraAttributes {
private:
	static RendererCameraAttributes *singleton;

	struct CameraAttributes {
		float exposure_multiplier = 1.0;
		float exposure_normalization = 1.0;
		float exposure_sensitivity = 100.0; // In ISO.

		bool use_auto_exposure = false;
		float auto_exposure_min_sensitivity = 50.0;
		float auto_exposure_max_sensitivity = 800.0;
		float auto_exposure_adjust_speed = 1.0;
		float auto_exposure_scale = 1.0;
		uint64_t auto_exposure_version = 0;
		// Mariner G2: exposure from a histogram of the log luminance.
		bool auto_exposure_histogram = false;
		float auto_exposure_low_percentile = 0.5; // The darker part of the frame left out (fraction of the weight).
		float auto_exposure_high_percentile = 0.98; // The brighter part above it left out.
		float auto_exposure_center_weight = 0.5; // 0: the whole frame alike, 1: a Gaussian around the center.
		float auto_exposure_speed_dark = 0.25; // Adaptation to a darker scene (auto_exposure_adjust_speed: to a brighter).

		bool dof_blur_far_enabled = false;
		float dof_blur_far_distance = 10;
		float dof_blur_far_transition = 5;
		bool dof_blur_near_enabled = false;
		float dof_blur_near_distance = 2;
		float dof_blur_near_transition = 1;
		float dof_blur_amount = 0.1;

		// Mariner E15a: motion blur.
		bool motion_blur_enabled = false;
		float motion_blur_intensity = 0.5; // Fraction of the motion of one frame (shutter angle / 360).
		float motion_blur_camera_motion = 1.0; // Weight of the camera's own motion (0: objects only).
		float motion_blur_max_length = 0.04; // Longest blur, fraction of the view height.

		// Mariner E15b: lens flare and lens dirt.
		bool lens_flare_enabled = false;
		float lens_flare_intensity = 0.02; // Of the light above the threshold, for the ghosts and the halo.
		float lens_flare_threshold = 4.0; // Luminance (scene linear) where the flare starts.
		int lens_flare_ghosts = 4;
		float lens_flare_ghost_spacing = 0.35;
		float lens_flare_halo_width = 0.45;
		float lens_flare_chromatic = 0.006;
		float lens_flare_sun_streaks = 1.0; // Starburst of the sun.
		int lens_flare_streak_count = 6;
		RID lens_dirt_texture;
		float lens_dirt_intensity = 1.0;

		// Mariner E23: image effects (Forward+).
		float chromatic_aberration = 0.0; // Red/blue offset at the corners, 1.0 = 2 % of the image.
		float lens_distortion = 0.0; // Barrel > 0, pincushion < 0.
		float film_grain = 0.0;
		float film_grain_size = 1.5; // Pixels.
		float sharpening = 0.0; // AMD FidelityFX CAS, 1.0 = its maximum.
	};

	RSE::DOFBlurQuality dof_blur_quality = RSE::DOF_BLUR_QUALITY_MEDIUM;
	RSE::DOFBokehShape dof_blur_bokeh_shape = RSE::DOF_BOKEH_HEXAGON;
	bool dof_blur_use_jitter = false;
	static uint64_t auto_exposure_counter;

	mutable RID_Owner<CameraAttributes, true> camera_attributes_owner;

public:
	// Mariner E15b: lens flare settings of a camera attributes.
	struct LensFlare {
		float intensity;
		float threshold;
		int ghosts;
		float ghost_spacing;
		float halo_width;
		float chromatic;
		float sun_streaks;
		int streak_count;
		RID dirt_texture;
		float dirt_intensity;
	};

	// Mariner E23: image effects of a camera attributes.
	struct ImageEffects {
		float chromatic_aberration = 0.0;
		float lens_distortion = 0.0;
		float film_grain = 0.0;
		float film_grain_size = 1.5;
		float sharpening = 0.0;
	};

	static RendererCameraAttributes *get_singleton() { return singleton; }

	RendererCameraAttributes();
	~RendererCameraAttributes();

	CameraAttributes *get_camera_attributes(RID p_rid) { return camera_attributes_owner.get_or_null(p_rid); }
	bool owns_camera_attributes(RID p_rid) { return camera_attributes_owner.owns(p_rid); }

	RID camera_attributes_allocate();
	void camera_attributes_initialize(RID p_rid);
	void camera_attributes_free(RID p_rid);

	void camera_attributes_set_dof_blur_quality(RSE::DOFBlurQuality p_quality, bool p_use_jitter);
	void camera_attributes_set_dof_blur_bokeh_shape(RSE::DOFBokehShape p_shape);

	void camera_attributes_set_dof_blur(RID p_camera_attributes, bool p_far_enable, float p_far_distance, float p_far_transition, bool p_near_enable, float p_near_distance, float p_near_transition, float p_amount);
	bool camera_attributes_get_dof_far_enabled(RID p_camera_attributes);
	float camera_attributes_get_dof_far_distance(RID p_camera_attributes);
	float camera_attributes_get_dof_far_transition(RID p_camera_attributes);
	bool camera_attributes_get_dof_near_enabled(RID p_camera_attributes);
	float camera_attributes_get_dof_near_distance(RID p_camera_attributes);
	float camera_attributes_get_dof_near_transition(RID p_camera_attributes);
	float camera_attributes_get_dof_blur_amount(RID p_camera_attributes);

	// Mariner E15a.
	void camera_attributes_set_motion_blur(RID p_camera_attributes, bool p_enable, float p_intensity, float p_camera_motion, float p_max_length);
	float camera_attributes_get_motion_blur_intensity(RID p_camera_attributes);
	float camera_attributes_get_motion_blur_camera_motion(RID p_camera_attributes);
	float camera_attributes_get_motion_blur_max_length(RID p_camera_attributes);
	_FORCE_INLINE_ bool camera_attributes_uses_motion_blur(RID p_camera_attributes) {
		CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);
		return cam_attributes && cam_attributes->motion_blur_enabled && cam_attributes->motion_blur_intensity > 0.0 && cam_attributes->motion_blur_max_length > 0.0;
	}

	// Mariner E15b.
	void camera_attributes_set_lens_flare(RID p_camera_attributes, bool p_enable, float p_intensity, float p_threshold, int p_ghosts, float p_ghost_spacing, float p_halo_width, float p_chromatic, float p_sun_streaks, int p_streak_count);
	void camera_attributes_set_lens_dirt(RID p_camera_attributes, RID p_texture, float p_intensity);
	bool camera_attributes_get_lens_flare(RID p_camera_attributes, LensFlare &r_lens_flare);

	// Mariner E23.
	void camera_attributes_set_image_effects(RID p_camera_attributes, float p_chromatic_aberration, float p_lens_distortion, float p_film_grain, float p_film_grain_size, float p_sharpening);
	ImageEffects camera_attributes_get_image_effects(RID p_camera_attributes) const;

	_FORCE_INLINE_ bool camera_attributes_uses_dof(RID p_camera_attributes) {
		CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);

		return cam_attributes && (cam_attributes->dof_blur_near_enabled || cam_attributes->dof_blur_far_enabled) && cam_attributes->dof_blur_amount > 0.0;
	}

	void camera_attributes_set_exposure(RID p_camera_attributes, float p_multiplier, float p_exposure_normalization);
	float camera_attributes_get_exposure_normalization_factor(RID p_camera_attributes);

	void camera_attributes_set_auto_exposure(RID p_camera_attributes, bool p_enable, float p_min_sensitivity, float p_max_sensitivity, float p_speed, float p_scale);
	float camera_attributes_get_auto_exposure_min_sensitivity(RID p_camera_attributes);
	float camera_attributes_get_auto_exposure_max_sensitivity(RID p_camera_attributes);
	float camera_attributes_get_auto_exposure_adjust_speed(RID p_camera_attributes);
	float camera_attributes_get_auto_exposure_scale(RID p_camera_attributes);
	uint64_t camera_attributes_get_auto_exposure_version(RID p_camera_attributes);
	// Mariner G2.
	struct AutoExposureHistogram {
		bool enabled = false;
		float low_percentile = 0.5;
		float high_percentile = 0.98;
		float center_weight = 0.5;
		float speed_dark = 0.25;
	};
	void camera_attributes_set_auto_exposure_histogram(RID p_camera_attributes, bool p_histogram, float p_low_percentile, float p_high_percentile, float p_center_weight, float p_speed_dark);
	AutoExposureHistogram camera_attributes_get_auto_exposure_histogram(RID p_camera_attributes);

	_FORCE_INLINE_ bool camera_attributes_uses_auto_exposure(RID p_camera_attributes) {
		CameraAttributes *cam_attributes = camera_attributes_owner.get_or_null(p_camera_attributes);

		return cam_attributes && cam_attributes->use_auto_exposure;
	}

	_FORCE_INLINE_ RSE::DOFBlurQuality camera_attributes_get_dof_blur_quality() {
		return dof_blur_quality;
	}

	_FORCE_INLINE_ RSE::DOFBokehShape camera_attributes_get_dof_blur_bokeh_shape() {
		return dof_blur_bokeh_shape;
	}

	_FORCE_INLINE_ bool camera_attributes_get_dof_blur_use_jitter() {
		return dof_blur_use_jitter;
	}
};
