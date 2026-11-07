/**************************************************************************/
/*  ddgi_volume_3d.cpp                                                    */
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

#include "ddgi_volume_3d.h"

#include "core/object/class_db.h"
#include "scene/resources/3d/world_3d.h"
#include "servers/rendering/rendering_server.h"
#include "servers/rendering/storage/environment_storage.h"

void DDGIVolume3D::_unregister() {
	if (registered && scenario.is_valid()) {
		RS::get_singleton()->scenario_set_ddgi_volume(scenario, false, Transform3D(), size, probe_spacing, rays_per_probe, probes_per_frame);
	}
	registered = false;
	scenario = RID();
}

void DDGIVolume3D::_update(bool p_force) {
	if (!is_inside_tree() || !is_visible_in_tree()) {
		_unregister();
		return;
	}
	const Transform3D xform = is_physics_interpolated_and_enabled() ? get_global_transform_interpolated() : get_global_transform();
	const RID new_scenario = get_world_3d().is_valid() ? get_world_3d()->get_scenario() : RID();
	if (new_scenario != scenario) {
		_unregister();
		scenario = new_scenario;
		p_force = true;
	}
	if (scenario.is_null() || (!p_force && registered && xform == last_transform)) {
		return;
	}
	last_transform = xform;
	registered = true;
	RS::get_singleton()->scenario_set_ddgi_volume(scenario, true, xform, size, probe_spacing, rays_per_probe, probes_per_frame);
}

void DDGIVolume3D::_notification(int p_what) {
	switch (p_what) {
		case NOTIFICATION_ENTER_WORLD:
		case NOTIFICATION_VISIBILITY_CHANGED: {
			_update(true);
		} break;
		case NOTIFICATION_INTERNAL_PROCESS: {
			// Every frame: the ship moves (and physics interpolation moves it between physics ticks).
			_update(false);
		} break;
		case NOTIFICATION_EXIT_WORLD: {
			_unregister();
		} break;
	}
}

void DDGIVolume3D::set_size(const Vector3 &p_size) {
	size = p_size.maxf(0.1);
	update_gizmos();
	_update(true);
}

Vector3 DDGIVolume3D::get_size() const {
	return size;
}

void DDGIVolume3D::set_probe_spacing(float p_spacing) {
	probe_spacing = CLAMP(p_spacing, 0.1f, 64.0f);
	_update(true);
	update_configuration_warnings();
}

float DDGIVolume3D::get_probe_spacing() const {
	return probe_spacing;
}

void DDGIVolume3D::set_rays_per_probe(int p_rays) {
	rays_per_probe = CLAMP((p_rays + 31) / 32 * 32, 32, 256);
	_update(true);
}

int DDGIVolume3D::get_rays_per_probe() const {
	return rays_per_probe;
}

void DDGIVolume3D::set_probes_per_frame(int p_probes) {
	probes_per_frame = MAX(p_probes, 0);
	_update(true);
}

int DDGIVolume3D::get_probes_per_frame() const {
	return probes_per_frame;
}

Vector3i DDGIVolume3D::probe_count_for(const Vector3 &p_size, float p_spacing) {
	return RendererEnvironmentStorage::ddgi_volume_probe_count(p_size, p_spacing);
}

AABB DDGIVolume3D::get_aabb() const {
	return AABB(-size * 0.5, size);
}

PackedStringArray DDGIVolume3D::get_configuration_warnings() const {
	PackedStringArray warnings = Node3D::get_configuration_warnings();
	const Vector3i count = probe_count_for(size, probe_spacing);
	const Vector3 effective = size / Vector3(count - Vector3i(1, 1, 1)).maxf(1.0);
	if (effective.x > probe_spacing * 1.01f || effective.y > probe_spacing * 1.01f || effective.z > probe_spacing * 1.01f) {
		warnings.push_back(vformat(RTR("Too many probes for the DDGI atlas: %d x %d x %d probes, spacing %.2f x %.2f x %.2f m instead of %.2f m."), count.x, count.y, count.z, effective.x, effective.y, effective.z, probe_spacing));
	}
	return warnings;
}

void DDGIVolume3D::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_size", "size"), &DDGIVolume3D::set_size);
	ClassDB::bind_method(D_METHOD("get_size"), &DDGIVolume3D::get_size);
	ClassDB::bind_method(D_METHOD("set_probe_spacing", "spacing"), &DDGIVolume3D::set_probe_spacing);
	ClassDB::bind_method(D_METHOD("get_probe_spacing"), &DDGIVolume3D::get_probe_spacing);
	ClassDB::bind_method(D_METHOD("set_rays_per_probe", "rays"), &DDGIVolume3D::set_rays_per_probe);
	ClassDB::bind_method(D_METHOD("get_rays_per_probe"), &DDGIVolume3D::get_rays_per_probe);
	ClassDB::bind_method(D_METHOD("set_probes_per_frame", "probes"), &DDGIVolume3D::set_probes_per_frame);
	ClassDB::bind_method(D_METHOD("get_probes_per_frame"), &DDGIVolume3D::get_probes_per_frame);
	ClassDB::bind_static_method("DDGIVolume3D", D_METHOD("probe_count_for", "size", "spacing"), &DDGIVolume3D::probe_count_for);

	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "size", PROPERTY_HINT_NONE, "suffix:m"), "set_size", "get_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "probe_spacing", PROPERTY_HINT_RANGE, "0.1,64,0.01,suffix:m"), "set_probe_spacing", "get_probe_spacing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "rays_per_probe", PROPERTY_HINT_RANGE, "32,256,32"), "set_rays_per_probe", "get_rays_per_probe");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "probes_per_frame", PROPERTY_HINT_RANGE, "0,16384,1,or_greater"), "set_probes_per_frame", "get_probes_per_frame");
}

DDGIVolume3D::DDGIVolume3D() {
	set_notify_transform(false);
	set_process_internal(true);
}

DDGIVolume3D::~DDGIVolume3D() {
	if (registered && scenario.is_valid() && RS::get_singleton()) {
		RS::get_singleton()->scenario_set_ddgi_volume(scenario, false, Transform3D(), size, probe_spacing, rays_per_probe, probes_per_frame);
	}
}
