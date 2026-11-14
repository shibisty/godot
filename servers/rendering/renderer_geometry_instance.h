/**************************************************************************/
/*  renderer_geometry_instance.h                                          */
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

#include "core/math/color.h"
#include "core/math/rect2.h"
#include "core/math/transform_3d.h"
#include "core/templates/rid.h"
#include "servers/rendering/storage/utilities.h"

// API definition for our RenderGeometryInstance class so we can expose this through GDExtension in the near future
class RenderGeometryInstance {
public:
	virtual ~RenderGeometryInstance() {}

	virtual void _mark_dirty() = 0;

	virtual void set_skeleton(RID p_skeleton) = 0;
	virtual void set_material_override(RID p_override) = 0;
	virtual void set_material_overlay(RID p_overlay) = 0;
	virtual void set_surface_materials(const Vector<RID> &p_materials) = 0;
	virtual void set_mesh_instance(RID p_mesh_instance) = 0;
	virtual void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) = 0;
	virtual void set_pivot_data(float p_sorting_offset, bool p_use_aabb_center) = 0;
	virtual void set_lod_bias(float p_lod_bias) = 0;
	virtual void set_layer_mask(uint32_t p_layer_mask) = 0;
	virtual void set_fade_range(bool p_enable_near, float p_near_begin, float p_near_end, bool p_enable_far, float p_far_begin, float p_far_end) = 0;
	virtual void set_parent_fade_alpha(float p_alpha) = 0;
	virtual void set_transparency(float p_transparency) = 0;
	virtual void set_use_baked_light(bool p_enable) = 0;
	virtual void set_use_dynamic_gi(bool p_enable) = 0;
	virtual void set_use_lightmap(RID p_lightmap_instance, const Rect2 &p_lightmap_uv_scale, int p_lightmap_slice_index) = 0;
	virtual void set_lightmap_capture(const Color *p_sh9) = 0;
	virtual void set_instance_shader_uniforms_offset(int32_t p_offset) = 0;
	virtual void set_cast_double_sided_shadows(bool p_enable) = 0;
	// Mariner E12 DG-F: only in the raytracing scene (not drawn); share of rays the instance stops there (1: opaque).
	virtual void set_raytracing_data(bool p_only, float p_opacity) {}
	// Mariner DG-M: share of the light the instance stops that is scattered onward (a scattering medium: canopy).
	virtual void set_raytracing_scattering(float p_opacity) {}
	// Mariner E25b: GeometryInstance3D.ignore_occlusion_culling, for the occlusion culling done by the renderer.
	virtual void set_ignore_occlusion_culling(bool p_enable) {}
	// Mariner E25c: visibility range (fade disabled) applied per instance by the renderer's GPU culling of an indirect
	// multimesh with view culling (GPU static batches); all 0: none.
	virtual void set_gpu_visibility_range(float p_begin, float p_end, float p_begin_margin, float p_end_margin) {}

	virtual void reset_motion_vectors() = 0;

	virtual Transform3D get_transform() = 0;
	virtual AABB get_aabb() = 0;

	virtual void clear_light_instances() = 0;
	virtual void pair_light_instance(const RID p_light_instance, RSE::LightType light_type, uint32_t placement_idx) = 0;
	virtual void pair_reflection_probe_instances(const RID *p_reflection_probe_instances, uint32_t p_reflection_probe_instance_count) = 0;
	virtual void pair_decal_instances(const RID *p_decal_instances, uint32_t p_decal_instance_count) = 0;
	virtual void pair_voxel_gi_instances(const RID *p_voxel_gi_instances, uint32_t p_voxel_gi_instance_count) = 0;

	virtual void set_softshadow_projector_pairing(bool p_softshadow, bool p_projector) = 0;
};

// Base implementation of RenderGeometryInstance shared by internal renderers.
class RenderGeometryInstanceBase : public RenderGeometryInstance {
public:
	// setup
	uint32_t base_flags = 0;
	uint32_t flags_cache = 0;

	// used during rendering
	float depth = 0;

	RID mesh_instance;

	Transform3D transform;
	bool mirror = false;
	AABB transformed_aabb;
	bool non_uniform_scale = false;
	float lod_model_scale = 1.0;
	float lod_bias = 0.0;
	float sorting_offset = 0.0;
	bool use_aabb_center = true;

	uint32_t layer_mask = 1;

	bool fade_near = false;
	float fade_near_begin = 0;
	float fade_near_end = 0;
	bool fade_far = false;
	float fade_far_begin = 0;
	float fade_far_end = 0;

	float parent_fade_alpha = 1.0;
	float force_alpha = 1.0;
	float raytracing_opacity = 1.0; // Mariner E12 DG-F.
	float raytracing_scattering = 0.0; // Mariner DG-M.
	bool raytracing_only = false;
	bool ignore_occlusion_culling = false; // Mariner E25b.
	float gpu_visibility_range[4] = {}; // Mariner E25c: begin, end, begin margin, end margin.

	int32_t shader_uniforms_offset = -1;

	struct Data {
		//data used less often goes into regular heap
		RID base;
		RSE::InstanceType base_type;

		RID skeleton;
		Vector<RID> surface_materials;
		RID material_override;
		RID material_overlay;
		AABB aabb;

		bool use_baked_light = false;
		bool use_dynamic_gi = false;
		bool cast_double_sided_shadows = false;
		bool dirty_dependencies = false;

		DependencyTracker dependency_tracker;
	};

	Data *data = nullptr;

	virtual void set_skeleton(RID p_skeleton) override;
	virtual void set_material_override(RID p_override) override;
	virtual void set_material_overlay(RID p_overlay) override;
	virtual void set_surface_materials(const Vector<RID> &p_materials) override;
	virtual void set_mesh_instance(RID p_mesh_instance) override;
	virtual void set_transform(const Transform3D &p_transform, const AABB &p_aabb, const AABB &p_transformed_aabb) override;
	virtual void set_pivot_data(float p_sorting_offset, bool p_use_aabb_center) override;
	virtual void set_lod_bias(float p_lod_bias) override;
	virtual void set_layer_mask(uint32_t p_layer_mask) override;
	virtual void set_fade_range(bool p_enable_near, float p_near_begin, float p_near_end, bool p_enable_far, float p_far_begin, float p_far_end) override;
	virtual void set_parent_fade_alpha(float p_alpha) override;
	virtual void set_transparency(float p_transparency) override;
	virtual void set_use_baked_light(bool p_enable) override;
	virtual void set_use_dynamic_gi(bool p_enable) override;
	virtual void set_instance_shader_uniforms_offset(int32_t p_offset) override;
	virtual void set_cast_double_sided_shadows(bool p_enable) override;
	virtual void set_raytracing_data(bool p_only, float p_opacity) override {
		raytracing_only = p_only;
		raytracing_opacity = p_opacity;
	}
	virtual void set_raytracing_scattering(float p_opacity) override {
		raytracing_scattering = p_opacity;
	}
	virtual void set_ignore_occlusion_culling(bool p_enable) override {
		ignore_occlusion_culling = p_enable;
	}
	virtual void set_gpu_visibility_range(float p_begin, float p_end, float p_begin_margin, float p_end_margin) override {
		gpu_visibility_range[0] = p_begin;
		gpu_visibility_range[1] = p_end;
		gpu_visibility_range[2] = p_begin_margin;
		gpu_visibility_range[3] = p_end_margin;
	}
	_FORCE_INLINE_ bool has_gpu_visibility_range() const { return gpu_visibility_range[0] > 0.0f || gpu_visibility_range[1] > 0.0f; }

	virtual void reset_motion_vectors() override;

	virtual Transform3D get_transform() override;
	virtual AABB get_aabb() override;
};
