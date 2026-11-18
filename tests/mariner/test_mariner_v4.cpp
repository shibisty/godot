/**************************************************************************/
/*  test_mariner_v4.cpp                                                   */
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

TEST_FORCE_LINK(test_mariner_v4)

#include "core/config/project_settings.h"
#include "core/object/class_db.h"
#include "core/object/method_bind.h"
#include "servers/rendering/rendering_server.h"

// Mariner engine patch V4: streaming of the cluster LOD into a GPU pool. Here: the project setting and the stats
// method. Streaming itself (convergence to the full mesh, the pool budget, eviction) is checked on lavapipe by
// engine/tests/v4/cluster_stream.gd.

namespace TestMarinerV4 {

TEST_CASE("[Mariner][V4] cluster streaming setting and stats") {
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/mesh_lod/cluster_streaming/pool_size_mb"));
	CHECK(int(ProjectSettings::get_singleton()->get_setting("rendering/mesh_lod/cluster_streaming/pool_size_mb")) == 0); // Off by default.
	MethodBind *method = ClassDB::get_method("RenderingServer", "cluster_streaming_get_stats");
	REQUIRE(method != nullptr);
	CHECK(method->get_argument_count() == 0);
	CHECK(method->is_const());
}

} // namespace TestMarinerV4
