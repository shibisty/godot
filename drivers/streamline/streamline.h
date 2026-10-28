/**************************************************************************/
/*  streamline.h                                                          */
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

#include "core/object/class_db.h" // IWYU pragma: keep
#include "core/object/object.h"
#include "core/os/thread_safe.h"
#include "core/variant/binder_common.h" // IWYU pragma: keep
#include "core/variant/variant.h"
#include "drivers/streamline/streamline_data.h"

// NVIDIA Streamline integration (ported from NVIDIA's Godot fork): DLSS Super Resolution / DLAA (E2), DLSS Frame
// Generation, Reflex and PCL latency markers (E3, game only). The singleton is always registered; on builds or
// systems without Streamline every capability reports false.
class Streamline : public Object {
	GDCLASS(Streamline, Object);
	_THREAD_SAFE_CLASS_
protected:
	static void _bind_methods();
	static Streamline *singleton;

	// Requested values, kept on every platform so scripts can read them back (applied when Streamline runs).
	StreamlineReflexMode reflex_mode = STREAMLINE_REFLEX_MODE_OFF;
	uint32_t reflex_frame_limit_us = 0;
	String dlss_preset = "?";
	int frame_generation_frames = 1;

public:
	static Streamline *get_singleton();
	static void register_singleton();

	void emit_marker(StreamlineMarkerType p_marker);
	void set_parameter(StreamlineParameterType p_parameter_type, const Variant &p_value);
	Variant get_parameter(StreamlineParameterType p_parameter_type) const;
	bool get_capability(StreamlineCapabilityType p_capability_type);

	// Reflex mode actually applied (can be higher than requested: DLSS-G forces low latency). Off without Reflex.
	int get_reflex_mode() const;
	// DLSS Frame Generation is enabled on a viewport right now.
	bool is_frame_generation_active() const;
	// DLSSGState::numFramesToGenerateMax: 0 = unknown / unavailable, 1 = 2x only, 3 = up to 4x (multi frame generation).
	int get_frame_generation_max_frames() const;
	// Last DLSSGStatus bitmask reported while frame generation was active (0 = OK).
	int get_frame_generation_status() const;

	void set_internal_parameter(const char *p_key, void *p_value);
	void *get_internal_parameter(StreamlineInternalParameterType p_internal_parameter_type);

	void update_project_settings();

	Streamline();
	virtual ~Streamline();
};

VARIANT_ENUM_CAST(StreamlineParameterType);
VARIANT_ENUM_CAST(StreamlineCapabilityType);
VARIANT_ENUM_CAST(StreamlineReflexMode);
