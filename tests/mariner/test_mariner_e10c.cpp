/**************************************************************************/
/*  test_mariner_e10c.cpp                                                 */
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

TEST_FORCE_LINK(test_mariner_e10c)

#include "core/config/project_settings.h"
#include "servers/rendering/renderer_scene_render.h"

// Mariner engine patch E10c: scrolling the cascades of the directional shadow cache. Images on lavapipe:
// engine/tests/e10/shadow_scroll.gd (the same image as without scrolling along walks; redraws when static casters
// change), engine/tests/e10/shadow_cache.gd.

namespace TestMarinerE10c {

TEST_CASE("[Mariner][E10c] Directional shadow cache scrolling setting and shadow data") {
	CHECK(ProjectSettings::get_singleton()->has_setting("rendering/lights_and_shadows/directional_shadow/cache_scrolling"));
	CHECK(bool(GLOBAL_GET("rendering/lights_and_shadows/directional_shadow/cache_scrolling")) == true);
	RendererSceneRender::RenderShadowData data;
	CHECK_FALSE(data.scroll);
	CHECK(data.scroll_extra_instances.size() == 0);
}

} // namespace TestMarinerE10c
