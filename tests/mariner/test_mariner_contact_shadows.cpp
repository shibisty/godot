/**************************************************************************/
/*  test_mariner_contact_shadows.cpp                                      */
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

TEST_FORCE_LINK(test_mariner_contact_shadows)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "scene/3d/light_3d.h"

// Mariner engine patch E11: screen space contact shadows of the sun (Forward+): a compute pass over the depth pre-pass
// marches a short ray towards the light per pixel; the opaque pass takes min(shadow, mask) for that light. Here: API
// and settings. The image (only next to the casters, sky untouched, longer rays, needs a shadowed light) is checked
// with a real renderer by engine/tests/e11/contact_shadows.gd.

namespace TestMarinerContactShadows {

TEST_CASE("[Mariner][E11] DirectionalLight3D.contact_shadow_length and RenderingServer method") {
	PropertyInfo pi;
	REQUIRE(ClassDB::get_property_info("DirectionalLight3D", "contact_shadow_length", &pi));
	CHECK(pi.type == Variant::FLOAT);
	CHECK(pi.hint == PROPERTY_HINT_RANGE);
	bool valid = false;
	CHECK(double(ClassDB::class_get_default_property_value("DirectionalLight3D", "contact_shadow_length", &valid)) == 0.0);
	CHECK(valid);

	DirectionalLight3D *light = memnew(DirectionalLight3D);
	light->set_contact_shadow_length(0.75);
	CHECK(light->get_contact_shadow_length() == doctest::Approx(0.75));
	light->set_contact_shadow_length(-2.0);
	CHECK(light->get_contact_shadow_length() == 0.0);
	memdelete(light);

	MethodBind *m = ClassDB::get_method("RenderingServer", "light_directional_set_contact_shadow");
	REQUIRE(m != nullptr);
	CHECK(m->get_argument_count() == 2);
	CHECK(m->get_argument_type(0) == Variant::RID);
	CHECK(m->get_argument_type(1) == Variant::FLOAT);
}

TEST_CASE("[Mariner][E11] Project settings") {
	struct Setting {
		const char *name;
		Variant value;
	};
	const Setting settings[] = {
		{ "rendering/lights_and_shadows/contact_shadows/steps", 16 },
		{ "rendering/lights_and_shadows/contact_shadows/thickness", 0.02 },
		{ "rendering/lights_and_shadows/contact_shadows/max_distance", 60.0 },
	};
	for (const Setting &setting : settings) {
		INFO(setting.name);
		REQUIRE(ProjectSettings::get_singleton()->has_setting(setting.name));
		CHECK(ProjectSettings::get_singleton()->property_get_revert(setting.name) == setting.value);
	}
}

} // namespace TestMarinerContactShadows
