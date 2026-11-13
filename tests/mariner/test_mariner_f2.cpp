/**************************************************************************/
/*  test_mariner_f2.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_f2)

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_rd/effects/gather_dof.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch F2: gathered depth of field (RendererRD::GatherDOF, gather_dof.glsl; images on lavapipe:
// engine/tests/f/dof.gd).

namespace TestMarinerF2 {

TEST_CASE("[Mariner][F2] Depth of field mode setting") {
	ProjectSettings *ps = ProjectSettings::get_singleton();
	const String name = "rendering/camera/depth_of_field/depth_of_field_mode";
	REQUIRE(ps->has_setting(name));
	CHECK(int(ps->property_get_revert(name)) == 1); // Gather by default.
	bool found = false;
	List<PropertyInfo> props;
	ps->get_property_list(&props);
	for (const PropertyInfo &pi : props) {
		if (pi.name == name) {
			found = true;
			CHECK(pi.hint == PROPERTY_HINT_ENUM);
			CHECK(pi.hint_string == "Bokeh (Classic),Gather");
		}
	}
	CHECK(found);
}

TEST_CASE("[Mariner][F2] Gather samples, aperture and push constant") {
	using RendererRD::GatherDOF;
	CHECK(GatherDOF::rings_for_quality(RSE::DOF_BLUR_QUALITY_VERY_LOW) == 2);
	CHECK(GatherDOF::rings_for_quality(RSE::DOF_BLUR_QUALITY_LOW) == 3);
	CHECK(GatherDOF::rings_for_quality(RSE::DOF_BLUR_QUALITY_MEDIUM) == 4);
	CHECK(GatherDOF::rings_for_quality(RSE::DOF_BLUR_QUALITY_HIGH) == 5);
	CHECK(GatherDOF::rings_for_quality(-3) == 2);
	CHECK(GatherDOF::rings_for_quality(17) == 5);
	CHECK(GatherDOF::sides_for_shape(RSE::DOF_BOKEH_BOX) == 4);
	CHECK(GatherDOF::sides_for_shape(RSE::DOF_BOKEH_HEXAGON) == 6);
	CHECK(GatherDOF::sides_for_shape(RSE::DOF_BOKEH_CIRCLE) == 0);
	CHECK(sizeof(GatherDOF::PushConstant) == 80); // Vulkan guarantees 128.
	CHECK(GatherDOF::TILE == 8); // TILE in gather_dof.glsl.
}

} // namespace TestMarinerF2
