/**************************************************************************/
/*  camera_attributes.h                                                   */
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

#include "core/io/resource.h"
#include "core/templates/rid.h"
#include "scene/resources/texture.h"

class CameraAttributes : public Resource {
	GDCLASS(CameraAttributes, Resource);

private:
	RID camera_attributes;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

	float exposure_multiplier = 1.0;
	float exposure_sensitivity = 100.0; // In ISO.
	void _update_exposure();

	bool auto_exposure_enabled = false;
	float auto_exposure_min = 0.01;
	float auto_exposure_max = 64.0;
	float auto_exposure_speed = 0.5;
	float auto_exposure_scale = 0.4;
	virtual void _update_auto_exposure() {}

	// Mariner E15a: motion blur.
	bool motion_blur_enabled = false;
	float motion_blur_intensity = 0.5;
	float motion_blur_camera_motion = 1.0;
	float motion_blur_max_length = 0.04;
	void _update_motion_blur();

	// Mariner E15b: lens flare and lens dirt.
	bool lens_flare_enabled = false;
	float lens_flare_intensity = 0.02;
	float lens_flare_threshold = 4.0;
	int lens_flare_ghosts = 4;
	float lens_flare_ghost_spacing = 0.35;
	float lens_flare_halo_width = 0.45;
	float lens_flare_chromatic = 0.006;
	float lens_flare_sun_streaks = 1.0;
	int lens_flare_streak_count = 6;
	float lens_dirt_intensity = 1.0;
	Ref<Texture2D> lens_dirt_texture;
	void _update_lens_flare();

	// Mariner E23: image effects.
	float image_chromatic_aberration = 0.0;
	float image_lens_distortion = 0.0;
	float image_film_grain = 0.0;
	float image_film_grain_size = 1.5;
	float image_sharpening = 0.0;
	void _update_image_effects();

public:
	virtual RID get_rid() const override;
	virtual float calculate_exposure_normalization() const { return 1.0; }

	void set_exposure_multiplier(float p_multiplier);
	float get_exposure_multiplier() const;
	void set_exposure_sensitivity(float p_sensitivity);
	float get_exposure_sensitivity() const;

	void set_auto_exposure_enabled(bool p_enabled);
	bool is_auto_exposure_enabled() const;
	void set_auto_exposure_speed(float p_auto_exposure_speed);
	float get_auto_exposure_speed() const;
	void set_auto_exposure_scale(float p_auto_exposure_scale);
	float get_auto_exposure_scale() const;

	void set_motion_blur_enabled(bool p_enabled);
	bool is_motion_blur_enabled() const;
	void set_motion_blur_intensity(float p_intensity);
	float get_motion_blur_intensity() const;
	void set_motion_blur_camera_motion(float p_amount);
	float get_motion_blur_camera_motion() const;
	void set_motion_blur_max_length(float p_length);
	float get_motion_blur_max_length() const;

	void set_image_chromatic_aberration(float p_value);
	float get_image_chromatic_aberration() const;
	void set_image_lens_distortion(float p_value);
	float get_image_lens_distortion() const;
	void set_image_film_grain(float p_value);
	float get_image_film_grain() const;
	void set_image_film_grain_size(float p_value);
	float get_image_film_grain_size() const;
	void set_image_sharpening(float p_value);
	float get_image_sharpening() const;

	void set_lens_flare_enabled(bool p_enabled);
	bool is_lens_flare_enabled() const;
	void set_lens_flare_intensity(float p_value);
	float get_lens_flare_intensity() const;
	void set_lens_flare_threshold(float p_value);
	float get_lens_flare_threshold() const;
	void set_lens_flare_ghosts(int p_value);
	int get_lens_flare_ghosts() const;
	void set_lens_flare_ghost_spacing(float p_value);
	float get_lens_flare_ghost_spacing() const;
	void set_lens_flare_halo_width(float p_value);
	float get_lens_flare_halo_width() const;
	void set_lens_flare_chromatic(float p_value);
	float get_lens_flare_chromatic() const;
	void set_lens_flare_sun_streaks(float p_value);
	float get_lens_flare_sun_streaks() const;
	void set_lens_flare_streak_count(int p_value);
	int get_lens_flare_streak_count() const;
	void set_lens_dirt_intensity(float p_value);
	float get_lens_dirt_intensity() const;
	void set_lens_dirt_texture(const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_lens_dirt_texture() const;

	CameraAttributes();
	~CameraAttributes();
};

class CameraAttributesPractical : public CameraAttributes {
	GDCLASS(CameraAttributesPractical, CameraAttributes);

private:
	// DOF blur
	bool dof_blur_far_enabled = false;
	float dof_blur_far_distance = 10.0;
	float dof_blur_far_transition = 5.0;

	bool dof_blur_near_enabled = false;
	float dof_blur_near_distance = 2.0;
	float dof_blur_near_transition = 1.0;

	float dof_blur_amount = 0.1;
	void _update_dof_blur();

	virtual void _update_auto_exposure() override;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;

public:
	// DOF blur
	void set_dof_blur_far_enabled(bool p_enabled);
	bool is_dof_blur_far_enabled() const;
	void set_dof_blur_far_distance(float p_distance);
	float get_dof_blur_far_distance() const;
	void set_dof_blur_far_transition(float p_distance);
	float get_dof_blur_far_transition() const;

	void set_dof_blur_near_enabled(bool p_enabled);
	bool is_dof_blur_near_enabled() const;
	void set_dof_blur_near_distance(float p_distance);
	float get_dof_blur_near_distance() const;
	void set_dof_blur_near_transition(float p_distance);
	float get_dof_blur_near_transition() const;
	void set_dof_blur_amount(float p_amount);
	float get_dof_blur_amount() const;

	void set_auto_exposure_min_sensitivity(float p_min);
	float get_auto_exposure_min_sensitivity() const;
	void set_auto_exposure_max_sensitivity(float p_max);
	float get_auto_exposure_max_sensitivity() const;

	virtual float calculate_exposure_normalization() const override;

	CameraAttributesPractical();
	~CameraAttributesPractical();
};

class CameraAttributesPhysical : public CameraAttributes {
	GDCLASS(CameraAttributesPhysical, CameraAttributes);

private:
	// Exposure
	float exposure_aperture = 16.0; // In f-stops;
	float exposure_shutter_speed = 100.0; // In 1 / seconds;

	// Camera properties.
	float frustum_focal_length = 35.0; // In millimeters.
	float frustum_focus_distance = 10.0; // In Meters.
	real_t frustum_near = 0.05;
	real_t frustum_far = 4000.0;
	real_t frustum_fov = 75.0;
	void _update_frustum();

	virtual void _update_auto_exposure() override;

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &property) const;

public:
	void set_aperture(float p_aperture);
	float get_aperture() const;

	void set_shutter_speed(float p_shutter_speed);
	float get_shutter_speed() const;

	void set_focal_length(float p_focal_length);
	float get_focal_length() const;

	void set_focus_distance(float p_focus_distance);
	float get_focus_distance() const;

	void set_near(real_t p_near);
	real_t get_near() const;

	void set_far(real_t p_far);
	real_t get_far() const;

	real_t get_fov() const;

	void set_auto_exposure_min_exposure_value(float p_min);
	float get_auto_exposure_min_exposure_value() const;
	void set_auto_exposure_max_exposure_value(float p_max);
	float get_auto_exposure_max_exposure_value() const;

	virtual float calculate_exposure_normalization() const override;

	CameraAttributesPhysical();
	~CameraAttributesPhysical();
};
