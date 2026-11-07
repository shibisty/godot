/**************************************************************************/
/*  test_mariner_e21.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e21)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "scene/3d/mesh_instance_3d.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch E21: translucent (coloured) shadows of the sun (GeometryInstance3D.shadow_translucent). Here:
// the property, the server call and the setting; the image (tinted light under glass and sailcloth, nothing above
// them, opaque shadow without the flag) on lavapipe by engine/tests/e21/translucent.gd.

namespace TestMarinerE21 {

TEST_CASE("[Mariner][E21] GeometryInstance3D.shadow_translucent") {
	MeshInstance3D *mi = memnew(MeshInstance3D);
	CHECK_FALSE(mi->is_shadow_translucent());
	mi->set_shadow_translucent(true);
	CHECK(mi->is_shadow_translucent());
	CHECK(bool(mi->get("shadow_translucent")));
	mi->set_shadow_translucent(false);
	CHECK_FALSE(mi->is_shadow_translucent());
	memdelete(mi);
	CHECK(ClassDB::has_method("RenderingServer", "instance_geometry_set_shadow_translucent"));
	CHECK(int(GLOBAL_GET("rendering/lights_and_shadows/directional_shadow/translucent_shadow_size")) == 1024);
	// The server call goes through without a GPU.
	RID instance = RS::get_singleton()->instance_create();
	RS::get_singleton()->instance_geometry_set_shadow_translucent(instance, true);
	RS::get_singleton()->free_rid(instance);
}

} // namespace TestMarinerE21
