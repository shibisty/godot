/**************************************************************************/
/*  test_mariner_g2.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_g2)

#include "core/object/class_db.h"
#include "scene/resources/camera_attributes.h"
#include "servers/rendering/renderer_rd/effects/luminance.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch G2: auto exposure from a histogram of the log luminance (CameraAttributes.auto_exposure_mode =
// Histogram): percentiles, center weight, separate adaptation toward darker scenes (images on lavapipe:
// engine/tests/g/auto_exposure.gd).

namespace TestMarinerG2 {

TEST_CASE("[Mariner][G2] CameraAttributes histogram auto exposure properties") {
	Ref<CameraAttributesPractical> attributes;
	attributes.instantiate();
	CHECK(attributes->get_auto_exposure_mode() == CameraAttributes::AUTO_EXPOSURE_MODE_AVERAGE);
	CHECK(attributes->get_auto_exposure_low_percentile() == doctest::Approx(0.5));
	CHECK(attributes->get_auto_exposure_high_percentile() == doctest::Approx(0.98));
	CHECK(attributes->get_auto_exposure_center_weight() == doctest::Approx(0.5));
	CHECK(attributes->get_auto_exposure_speed_dark() == doctest::Approx(0.25));

	attributes->set_auto_exposure_mode(CameraAttributes::AUTO_EXPOSURE_MODE_HISTOGRAM);
	CHECK(attributes->get_auto_exposure_mode() == CameraAttributes::AUTO_EXPOSURE_MODE_HISTOGRAM);
	// The low percentile stays below the high one.
	attributes->set_auto_exposure_low_percentile(0.99);
	CHECK(attributes->get_auto_exposure_high_percentile() >= attributes->get_auto_exposure_low_percentile() + 0.0099);
	attributes->set_auto_exposure_high_percentile(0.2);
	CHECK(attributes->get_auto_exposure_low_percentile() <= 0.1901);
	attributes->set_auto_exposure_center_weight(3.0);
	CHECK(attributes->get_auto_exposure_center_weight() == doctest::Approx(1.0));
	attributes->set_auto_exposure_speed_dark(-1.0);
	CHECK(attributes->get_auto_exposure_speed_dark() == doctest::Approx(0.0));

	for (const char *name : { "auto_exposure_mode", "auto_exposure_low_percentile", "auto_exposure_high_percentile", "auto_exposure_center_weight", "auto_exposure_speed_dark" }) {
		PropertyInfo pi;
		CHECK_MESSAGE(ClassDB::get_property_info("CameraAttributesPhysical", name, &pi), name);
	}
	MethodBind *m = ClassDB::get_method("RenderingServer", "camera_attributes_set_auto_exposure_histogram");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 6);
}

TEST_CASE("[Mariner][G2] Histogram range") {
	// The bins span the luminance clamp range and 2 stops on either side.
	float log_min = 0.0, log_range = 0.0;
	RendererRD::Luminance::histogram_log_range(0.0625, 1.0, log_min, log_range);
	CHECK(log_min == doctest::Approx(-6.0));
	CHECK(log_range == doctest::Approx(8.0));
	// A degenerate range still spans a stop (plus the margins).
	RendererRD::Luminance::histogram_log_range(1.0, 1.0, log_min, log_range);
	CHECK(log_range == doctest::Approx(5.0));
	CHECK(RendererRD::Luminance::HISTOGRAM_BINS == 64);
}

} // namespace TestMarinerG2
