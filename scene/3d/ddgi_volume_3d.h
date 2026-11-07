/**************************************************************************/
/*  ddgi_volume_3d.h                                                      */
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

#include "scene/3d/node_3d.h"

// Mariner E12 DG2: a DDGI probe grid in the coordinates of this node (a ship): the probes move and turn with it, so
// the light in its rooms stays valid when it sails, rolls and pitches. Sampled inside its box instead of the world grid
// of the Environment (DDGI must be enabled there). One per World3D.
class DDGIVolume3D : public Node3D {
	GDCLASS(DDGIVolume3D, Node3D);

	Vector3 size = Vector3(20, 8, 60);
	float probe_spacing = 1.0;
	int rays_per_probe = 128;
	int probes_per_frame = 0;
	RID scenario;
	Transform3D last_transform;
	bool registered = false;

	void _update(bool p_force);
	void _unregister();

protected:
	void _notification(int p_what);
	static void _bind_methods();

public:
	void set_size(const Vector3 &p_size);
	Vector3 get_size() const;
	void set_probe_spacing(float p_spacing);
	float get_probe_spacing() const;
	void set_rays_per_probe(int p_rays);
	int get_rays_per_probe() const;
	void set_probes_per_frame(int p_probes);
	int get_probes_per_frame() const;

	// Probes per axis and their spacing for a box (at most the DDGI atlas limits; the spacing grows to fit).
	static Vector3i probe_count_for(const Vector3 &p_size, float p_spacing);

	AABB get_aabb() const;
	PackedStringArray get_configuration_warnings() const override;

	DDGIVolume3D();
	~DDGIVolume3D();
};
