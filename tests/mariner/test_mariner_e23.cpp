/**************************************************************************/
/*  test_mariner_e23.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e23)

#include "core/object/class_db.h"
#include "scene/resources/camera_attributes.h"
#include "scene/resources/environment.h"
#include "servers/rendering/renderer_rd/effects/cas.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/environment_storage.h"

// Mariner engine patch E23: post set — Gran Turismo 7 tone mapper, film grain, chromatic aberration, lens distortion,
// contrast adaptive sharpening. Here: the enums, the properties and the CPU-side curve constants; the image (GT7 curve,
// grain statistics, colour fringes at the edges, sharpened edges) on lavapipe by engine/tests/e23/post.gd.

namespace TestMarinerE23 {

TEST_CASE("[Mariner][E23] Gran Turismo 7 tone mapper") {
	CHECK(int(Environment::TONE_MAPPER_GT7) == 5);
	CHECK(int(RSE::ENV_TONE_MAPPER_GT7) == 5);
	CHECK(ClassDB::get_integer_constant("Environment", "TONE_MAPPER_GT7") == 5);

	Ref<Environment> env;
	env.instantiate();
	env->set_tonemapper(Environment::TONE_MAPPER_GT7);
	CHECK(env->get_tonemapper() == Environment::TONE_MAPPER_GT7);

	// SDR: peak at GT7's paper white (250 cd/m^2 = 2.5 frame buffer units).
	float peak = 0.0f;
	float intensity = 0.0f;
	RendererEnvironmentStorage::gt7_tonemap_parameters(1.0f, peak, intensity);
	CHECK(peak == doctest::Approx(2.5f));
	// PQ of 250 cd/m^2 (ST 2084): 0.6026.
	CHECK(intensity == doctest::Approx(0.6026f).epsilon(0.002));
	// HDR output at 4x paper white: 1000 cd/m^2, PQ 0.7518.
	RendererEnvironmentStorage::gt7_tonemap_parameters(4.0f, peak, intensity);
	CHECK(peak == doctest::Approx(10.0f));
	CHECK(intensity == doctest::Approx(0.7518f).epsilon(0.002));
	// Below SDR and above the PQ range: clamped.
	RendererEnvironmentStorage::gt7_tonemap_parameters(0.5f, peak, intensity);
	CHECK(peak == doctest::Approx(2.5f));
	RendererEnvironmentStorage::gt7_tonemap_parameters(100.0f, peak, intensity);
	CHECK(peak == doctest::Approx(100.0f));
	CHECK(intensity == doctest::Approx(1.0f).epsilon(0.001));
}

TEST_CASE("[Mariner][E23] CameraAttributes image effects") {
	Ref<CameraAttributesPractical> attributes;
	attributes.instantiate();
	CHECK(attributes->get_image_chromatic_aberration() == 0.0f);
	CHECK(attributes->get_image_lens_distortion() == 0.0f);
	CHECK(attributes->get_image_film_grain() == 0.0f);
	CHECK(attributes->get_image_film_grain_size() == doctest::Approx(1.5f));
	CHECK(attributes->get_image_sharpening() == 0.0f);

	attributes->set_image_chromatic_aberration(2.0f);
	CHECK(attributes->get_image_chromatic_aberration() == 1.0f);
	attributes->set_image_lens_distortion(-3.0f);
	CHECK(attributes->get_image_lens_distortion() == -1.0f);
	attributes->set_image_film_grain(0.3f);
	CHECK(float(attributes->get("image_film_grain")) == doctest::Approx(0.3f));
	attributes->set_image_film_grain_size(10.0f);
	CHECK(attributes->get_image_film_grain_size() == doctest::Approx(3.99f));
	attributes->set_image_sharpening(0.5f);
	CHECK(attributes->get_image_sharpening() == doctest::Approx(0.5f));

	CHECK(ClassDB::has_method("RenderingServer", "camera_attributes_set_image_effects"));

	// CAS weights: none at 0, the CAS maximum (-1/5) at 1.
	CHECK(RendererRD::CAS::peak_for_amount(0.0f) == 0.0f);
	CHECK(RendererRD::CAS::peak_for_amount(1.0f) == doctest::Approx(-0.2f));
	CHECK(RendererRD::CAS::peak_for_amount(0.5f) == doctest::Approx(-0.5f / 6.5f));
}

} // namespace TestMarinerE23
