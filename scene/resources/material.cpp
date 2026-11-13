/**************************************************************************/
/*  material.cpp                                                          */
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

#include "material.h"

#include "core/config/engine.h"
#include "core/config/project_settings.h"
#include "core/error/error_macros.h"
#include "core/io/resource_loader.h"
#include "core/object/callable_mp.h"
#include "core/object/class_db.h"
#include "core/os/os.h"
#include "core/version.h"
#include "scene/main/scene_tree.h"
#include "scene/resources/alpha_mask_bake.h"
#include "scene/resources/texture.h"
#include "servers/rendering/rendering_server.h"

void Material::set_next_pass(const Ref<Material> &p_pass) {
	for (Ref<Material> pass_child = p_pass; pass_child.is_valid(); pass_child = pass_child->get_next_pass()) {
		ERR_FAIL_COND_MSG(pass_child == this, "Can't set as next_pass one of its parents to prevent crashes due to recursive loop.");
	}

	if (next_pass == p_pass) {
		return;
	}

	next_pass = p_pass;

	if (material.is_valid()) {
		RID next_pass_rid;
		if (next_pass.is_valid()) {
			next_pass_rid = next_pass->get_rid();
		}

		RS::get_singleton()->material_set_next_pass(material, next_pass_rid);
	}
}

Ref<Material> Material::get_next_pass() const {
	return next_pass;
}

void Material::set_render_priority(int p_priority) {
	ERR_FAIL_COND(p_priority < RENDER_PRIORITY_MIN);
	ERR_FAIL_COND(p_priority > RENDER_PRIORITY_MAX);

	render_priority = p_priority;

	if (material.is_valid()) {
		RS::get_singleton()->material_set_render_priority(material, p_priority);
	}
}

int Material::get_render_priority() const {
	return render_priority;
}

RID Material::get_rid() const {
	return material;
}

void Material::_validate_property(PropertyInfo &p_property) const {
	if (!_can_do_next_pass() && p_property.name == "next_pass") {
		p_property.usage = PROPERTY_USAGE_NONE;
	} else if (!_can_use_render_priority() && p_property.name == "render_priority") {
		p_property.usage = PROPERTY_USAGE_NONE;
	}
}

void Material::_mark_ready() {
	init_state = INIT_STATE_READY;
}

void Material::_mark_initialized(const Callable &p_add_to_dirty_list, const Callable &p_update_shader) {
	// If this is happening as part of resource loading, it is not safe to queue the update
	// as an addition to the dirty list. It would be if the load is happening on the main thread,
	// but even so we'd rather perform the update directly instead of using the dirty list.
	if (ResourceLoader::is_within_load()) {
		DEV_ASSERT(init_state != INIT_STATE_READY);
		if (init_state == INIT_STATE_UNINITIALIZED) { // Prevent queueing twice.
			if (p_update_shader.is_valid()) {
				init_state = INIT_STATE_INITIALIZING;
				callable_mp(this, &Material::_mark_ready).call_deferred();
				p_update_shader.call_deferred();
			} else {
				init_state = INIT_STATE_READY;
			}
		}
	} else {
		// Straightforward conditions.
		init_state = INIT_STATE_READY;
		p_add_to_dirty_list.call();
	}
}

void Material::inspect_native_shader_code() {
	SceneTree *st = Object::cast_to<SceneTree>(OS::get_singleton()->get_main_loop());
	RID shader = get_shader_rid();
	if (st && shader.is_valid()) {
		st->call_group_flags(SceneTree::GROUP_CALL_DEFERRED, "_native_shader_source_visualizer", "_inspect_shader", shader);
	}
}

RID Material::get_shader_rid() const {
	RID ret;
	GDVIRTUAL_CALL(_get_shader_rid, ret);
	return ret;
}
Shader::Mode Material::get_shader_mode() const {
	Shader::Mode ret = Shader::MODE_MAX;
	GDVIRTUAL_CALL(_get_shader_mode, ret);
	return ret;
}

bool Material::_can_do_next_pass() const {
	bool ret = false;
	GDVIRTUAL_CALL(_can_do_next_pass, ret);
	return ret;
}

bool Material::_can_use_render_priority() const {
	bool ret = false;
	GDVIRTUAL_CALL(_can_use_render_priority, ret);
	return ret;
}

Ref<Resource> Material::create_placeholder() const {
	Ref<PlaceholderMaterial> placeholder;
	placeholder.instantiate();
	return placeholder;
}

void Material::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_next_pass", "next_pass"), &Material::set_next_pass);
	ClassDB::bind_method(D_METHOD("get_next_pass"), &Material::get_next_pass);

	ClassDB::bind_method(D_METHOD("set_render_priority", "priority"), &Material::set_render_priority);
	ClassDB::bind_method(D_METHOD("get_render_priority"), &Material::get_render_priority);

	ClassDB::bind_method(D_METHOD("inspect_native_shader_code"), &Material::inspect_native_shader_code);
	ClassDB::set_method_flags(get_class_static(), StringName("inspect_native_shader_code"), METHOD_FLAGS_DEFAULT | METHOD_FLAG_EDITOR);

	ClassDB::bind_method(D_METHOD("create_placeholder"), &Material::create_placeholder);

	ADD_PROPERTY(PropertyInfo(Variant::INT, "render_priority", PROPERTY_HINT_RANGE, itos(RENDER_PRIORITY_MIN) + "," + itos(RENDER_PRIORITY_MAX) + ",1"), "set_render_priority", "get_render_priority");
	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "next_pass", PROPERTY_HINT_RESOURCE_TYPE, Material::get_class_static()), "set_next_pass", "get_next_pass");

	BIND_CONSTANT(RENDER_PRIORITY_MAX);
	BIND_CONSTANT(RENDER_PRIORITY_MIN);

	GDVIRTUAL_BIND(_get_shader_rid)
	GDVIRTUAL_BIND(_get_shader_mode)
	GDVIRTUAL_BIND(_can_do_next_pass)
	GDVIRTUAL_BIND(_can_use_render_priority)
}

Material::Material() {
	render_priority = 0;
}

Material::~Material() {
	if (material.is_valid()) {
		ERR_FAIL_NULL(RenderingServer::get_singleton());
		RenderingServer::get_singleton()->free_rid(material);
	}
}

///////////////////////////////////

bool ShaderMaterial::_set(const StringName &p_name, const Variant &p_value) {
	if (shader.is_valid()) {
		const StringName *sn = remap_cache.getptr(p_name);
		if (sn) {
			set_shader_parameter(*sn, p_value);
			return true;
		}
		String s = p_name;
		if (s.begins_with("shader_parameter/")) {
			String param = s.replace_first("shader_parameter/", "");
			remap_cache[s] = param;
			set_shader_parameter(param, p_value);
			return true;
		}
#ifndef DISABLE_DEPRECATED
		// Compatibility remaps are only needed here.
		if (s.begins_with("param/")) {
			s = s.replace_first("param/", "shader_parameter/");
		} else if (s.begins_with("shader_param/")) {
			s = s.replace_first("shader_param/", "shader_parameter/");
		} else if (s.begins_with("shader_uniform/")) {
			s = s.replace_first("shader_uniform/", "shader_parameter/");
		} else {
			return false; // Not a shader parameter.
		}

		WARN_PRINT("This material (containing shader with path: '" + shader->get_path() + "') uses an old deprecated parameter names. Consider re-saving this resource (or scene which contains it) in order for it to continue working in future versions.");
		String param = s.replace_first("shader_parameter/", "");
		remap_cache[s] = param;
		set_shader_parameter(param, p_value);
		return true;
#endif
	}

	return false;
}

bool ShaderMaterial::_get(const StringName &p_name, Variant &r_ret) const {
	if (shader.is_valid()) {
		const StringName *sn = remap_cache.getptr(p_name);
		if (sn) {
			// Only return a parameter if it was previously set.
			r_ret = get_shader_parameter(*sn);
			return true;
		}
	}

	return false;
}

void ShaderMaterial::_get_property_list(List<PropertyInfo> *p_list) const {
	if (shader.is_valid()) {
		List<PropertyInfo> list;
		shader->get_shader_uniform_list(&list, true);

		HashMap<String, List<PropertyInfo>> groups;
		LocalVector<String> vgroups;

		String last_group = "<None>";

		bool is_none_group_undefined = true;
		bool is_none_group = true;

		for (const PropertyInfo &pi : list) {
			if (pi.usage == PROPERTY_USAGE_GROUP) {
				if (!pi.name.is_empty()) {
					last_group = pi.name;
					is_none_group = false;

					if (!groups.has(last_group)) {
						PropertyInfo info;
						info.usage = PROPERTY_USAGE_GROUP;
						info.name = last_group.capitalize();
						info.hint_string = "shader_parameter/";

						List<PropertyInfo> props;
						props.push_back(info);

						groups.insert(last_group, props);
						vgroups.push_back(last_group);
					}
				} else {
					last_group = "<None>";
					is_none_group = true;
				}
				continue; // Pass group.
			}

			if (is_none_group_undefined && is_none_group) {
				is_none_group_undefined = false;

				PropertyInfo info;
				info.usage = PROPERTY_USAGE_GROUP;
				info.name = "Shader Parameters";
				info.hint_string = "shader_parameter/";

				List<PropertyInfo> props;
				props.push_back(info);

				groups.insert("<None>", props);
				vgroups.push_back("<None>");
			}

			const bool is_uniform_cached = param_cache.has(pi.name);
			bool is_uniform_type_compatible = true;

			if (is_uniform_cached) {
				// Check if the uniform Variant type changed, for example vec3 to vec4.
				const Variant &cached = param_cache.get(pi.name);

				if (cached.is_array()) {
					// Allow some array conversions for backwards compatibility.
					is_uniform_type_compatible = Variant::can_convert(pi.type, cached.get_type());
				} else {
					is_uniform_type_compatible = pi.type == cached.get_type();
				}

#ifndef DISABLE_DEPRECATED
				// PackedFloat32Array -> PackedVector4Array conversion.
				if (!is_uniform_type_compatible && pi.type == Variant::PACKED_VECTOR4_ARRAY && cached.get_type() == Variant::PACKED_FLOAT32_ARRAY) {
					PackedVector4Array varray;
					PackedFloat32Array array = (PackedFloat32Array)cached;

					for (int i = 0; i + 3 < array.size(); i += 4) {
						varray.push_back(Vector4(array[i], array[i + 1], array[i + 2], array[i + 3]));
					}

					param_cache.insert(pi.name, varray);
					is_uniform_type_compatible = true;
				}
#endif

				if (is_uniform_type_compatible && pi.type == Variant::OBJECT && cached.get_type() == Variant::OBJECT) {
					// Check if the Object class (hint string) changed, for example Texture2D sampler to Texture3D.
					// Allow inheritance, Texture2D type sampler should also accept CompressedTexture2D.
					Object *cached_obj = cached;
					if (!cached_obj->is_class(pi.hint_string)) {
						is_uniform_type_compatible = false;
					}
				}
			}

			PropertyInfo info = pi;
			info.name = "shader_parameter/" + info.name;
			if (!is_uniform_cached || !is_uniform_type_compatible) {
				// Property has never been edited or its type changed, retrieve with default value.
				Variant default_value = RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), pi.name);
				param_cache.insert(pi.name, default_value);
				remap_cache.insert(info.name, pi.name);
			}
			groups[last_group].push_back(info);
		}

		for (const String &group : vgroups) {
			List<PropertyInfo> &prop_infos = groups[group];
			for (const PropertyInfo &item : prop_infos) {
				p_list->push_back(item);
			}
		}
	}
}

bool ShaderMaterial::_property_can_revert(const StringName &p_name) const {
	if (shader.is_valid()) {
		if (remap_cache.has(p_name)) {
			return true;
		}
		const String sname = p_name;
		return sname == "render_priority" || sname == "next_pass";
	}
	return false;
}

bool ShaderMaterial::_property_get_revert(const StringName &p_name, Variant &r_property) const {
	if (shader.is_valid()) {
		const StringName *pr = remap_cache.getptr(p_name);
		if (pr) {
			r_property = RenderingServer::get_singleton()->shader_get_parameter_default(shader->get_rid(), *pr);
			return true;
		} else if (p_name == "render_priority") {
			r_property = 0;
			return true;
		} else if (p_name == "next_pass") {
			r_property = Variant();
			return true;
		}
	}
	return false;
}

void ShaderMaterial::set_shader(const Ref<Shader> &p_shader) {
	// Only connect/disconnect the signal when running in the editor.
	// This can be a slow operation, and `notify_property_list_changed()` (which is called by `_shader_changed()`)
	// does nothing in non-editor builds anyway. See GH-34741 for details.
	if (shader.is_valid() && Engine::get_singleton()->is_editor_hint()) {
		shader->disconnect_changed(callable_mp(this, &ShaderMaterial::_shader_changed));
	}

	shader = p_shader;

	RID rid;
	if (shader.is_valid()) {
		rid = shader->get_rid();

		if (Engine::get_singleton()->is_editor_hint()) {
			shader->connect_changed(callable_mp(this, &ShaderMaterial::_shader_changed));
		}
	}

	RID material_rid = _get_material();
	if (material_rid.is_valid()) {
		RS::get_singleton()->material_set_shader(material_rid, rid);
	}

	notify_property_list_changed(); //properties for shader exposed
	emit_changed();
}

Ref<Shader> ShaderMaterial::get_shader() const {
	return shader;
}

void ShaderMaterial::set_shader_parameter(const StringName &p_param, const Variant &p_value) {
	RID material_rid = _get_material();
	if (p_value.get_type() == Variant::NIL) {
		param_cache.erase(p_param);
		if (material_rid.is_valid()) {
			RS::get_singleton()->material_set_param(material_rid, p_param, Variant());
		}
	} else {
		Variant *v = param_cache.getptr(p_param);
		if (!v) {
			// Never assigned, also update the remap cache.
			remap_cache["shader_parameter/" + p_param.operator String()] = p_param;
			param_cache.insert(p_param, p_value);
		} else {
			*v = p_value;
		}

		if (p_value.get_type() == Variant::OBJECT) {
			RID tex_rid = p_value;
			if (tex_rid == RID()) {
				param_cache.erase(p_param);

				if (material_rid.is_valid()) {
					RS::get_singleton()->material_set_param(material_rid, p_param, Variant());
				}
			} else if (material_rid.is_valid()) {
				RS::get_singleton()->material_set_param(material_rid, p_param, tex_rid);
			}
		} else if (material_rid.is_valid()) {
			RS::get_singleton()->material_set_param(material_rid, p_param, p_value);
		}
	}
}

Variant ShaderMaterial::get_shader_parameter(const StringName &p_param) const {
	if (param_cache.has(p_param)) {
		return param_cache[p_param];
	} else {
		return Variant();
	}
}

void ShaderMaterial::_shader_changed() {
	notify_property_list_changed(); //update all properties
}

void ShaderMaterial::_check_material_rid() const {
	MutexLock lock(material_rid_mutex);
	if (_get_material().is_null()) {
		RID shader_rid = shader.is_valid() ? shader->get_rid() : RID();
		RID next_pass_rid;
		if (get_next_pass().is_valid()) {
			next_pass_rid = get_next_pass()->get_rid();
		}

		_set_material(RS::get_singleton()->material_create_from_shader(next_pass_rid, get_render_priority(), shader_rid));

		for (KeyValue<StringName, Variant> param : param_cache) {
			if (param.value.get_type() == Variant::OBJECT) {
				RID tex_rid = param.value;
				if (tex_rid.is_valid()) {
					RS::get_singleton()->material_set_param(_get_material(), param.key, tex_rid);
				} else {
					RS::get_singleton()->material_set_param(_get_material(), param.key, Variant());
				}
			} else {
				RS::get_singleton()->material_set_param(_get_material(), param.key, param.value);
			}
		}
	}
}

void ShaderMaterial::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_shader", "shader"), &ShaderMaterial::set_shader);
	ClassDB::bind_method(D_METHOD("get_shader"), &ShaderMaterial::get_shader);
	ClassDB::bind_method(D_METHOD("set_shader_parameter", "param", "value"), &ShaderMaterial::set_shader_parameter);
	ClassDB::bind_method(D_METHOD("get_shader_parameter", "param"), &ShaderMaterial::get_shader_parameter);

	ADD_PROPERTY(PropertyInfo(Variant::OBJECT, "shader", PROPERTY_HINT_RESOURCE_TYPE, Shader::get_class_static()), "set_shader", "get_shader");
}

#ifdef TOOLS_ENABLED
void ShaderMaterial::get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const {
	const String pf = p_function;
	if (p_idx == 0 && (pf == "get_shader_parameter" || pf == "set_shader_parameter")) {
		if (shader.is_valid()) {
			List<PropertyInfo> pl;
			shader->get_shader_uniform_list(&pl);
			for (const PropertyInfo &E : pl) {
				r_options->push_back(E.name.replace_first("shader_parameter/", "").quote());
			}
		}
	}
	Material::get_argument_options(p_function, p_idx, r_options);
}
#endif

bool ShaderMaterial::_can_do_next_pass() const {
	return shader.is_valid() && shader->get_mode() == Shader::MODE_SPATIAL;
}

bool ShaderMaterial::_can_use_render_priority() const {
	return shader.is_valid() && shader->get_mode() == Shader::MODE_SPATIAL;
}

Shader::Mode ShaderMaterial::get_shader_mode() const {
	if (shader.is_valid()) {
		return shader->get_mode();
	} else {
		return Shader::MODE_SPATIAL;
	}
}

RID ShaderMaterial::get_rid() const {
	_check_material_rid();
	return Material::get_rid();
}

RID ShaderMaterial::get_shader_rid() const {
	if (shader.is_valid()) {
		return shader->get_rid();
	} else {
		return RID();
	}
}

ShaderMaterial::ShaderMaterial() {
	// Material RID will be empty until it is required.
}

ShaderMaterial::~ShaderMaterial() {
}

/////////////////////////////////

HashMap<BaseMaterial3D::MaterialKey, BaseMaterial3D::ShaderData, BaseMaterial3D::MaterialKey> BaseMaterial3D::shader_map;
Mutex BaseMaterial3D::shader_map_mutex;
BaseMaterial3D::ShaderNames *BaseMaterial3D::shader_names = nullptr;
Mutex BaseMaterial3D::material_mutex;
SelfList<BaseMaterial3D>::List BaseMaterial3D::dirty_materials;

void BaseMaterial3D::init_shaders() {
	shader_names = memnew(ShaderNames);

	shader_names->albedo = "albedo";
	shader_names->specular = "specular";
	shader_names->roughness = "roughness";
	shader_names->metallic = "metallic";
	shader_names->emission = "emission";
	shader_names->emission_energy = "emission_energy";
	shader_names->normal_scale = "normal_scale";
	shader_names->rim = "rim";
	shader_names->rim_tint = "rim_tint";
	shader_names->clearcoat = "clearcoat";
	shader_names->clearcoat_roughness = "clearcoat_roughness";
	shader_names->anisotropy = "anisotropy_ratio";
	shader_names->heightmap_scale = "heightmap_scale";
	shader_names->subsurface_scattering_strength = "subsurface_scattering_strength";
	shader_names->backlight = "backlight";
	shader_names->refraction = "refraction";
	shader_names->point_size = "point_size";
	shader_names->uv1_scale = "uv1_scale";
	shader_names->uv1_offset = "uv1_offset";
	shader_names->uv2_scale = "uv2_scale";
	shader_names->uv2_offset = "uv2_offset";
	shader_names->uv1_blend_sharpness = "uv1_blend_sharpness";
	shader_names->uv2_blend_sharpness = "uv2_blend_sharpness";

	shader_names->particles_anim_h_frames = "particles_anim_h_frames";
	shader_names->particles_anim_v_frames = "particles_anim_v_frames";
	shader_names->particles_anim_loop = "particles_anim_loop";
	shader_names->heightmap_min_layers = "heightmap_min_layers";
	shader_names->heightmap_max_layers = "heightmap_max_layers";
	shader_names->heightmap_flip = "heightmap_flip";
	shader_names->heightmap_self_shadow_strength = "heightmap_self_shadow_strength";
	shader_names->sheen_color = "sheen_color";
	shader_names->sheen_roughness = "sheen_roughness";
	shader_names->wetness_amount = "wetness_amount"; // Mariner G4.
	shader_names->wetness_porosity = "wetness_porosity";
	shader_names->wetness_puddles = "wetness_puddles";
	shader_names->snow_amount = "snow_amount"; // Mariner G4b.
	shader_names->snow_color = "snow_color";
	shader_names->snow_roughness = "snow_roughness";
	shader_names->snow_max_slope = "snow_max_slope";
	shader_names->snow_relief = "snow_relief";
	shader_names->impostor_faces = "impostor_faces"; // Mariner G5c.
	shader_names->planar_reflection_amount = "planar_reflection_amount"; // Mariner G6.
	shader_names->planar_reflection_distortion = "planar_reflection_distortion";
	shader_names->displacement_scale = "displacement_scale"; // Mariner G7.
	shader_names->displacement_midlevel = "displacement_midlevel";
	shader_names->displacement_texture_lod = "displacement_texture_lod";
	shader_names->interior_rooms = "interior_rooms"; // Mariner G5a.
	shader_names->interior_room_size = "interior_room_size";
	shader_names->interior_atlas_size = "interior_atlas_size";
	shader_names->interior_back_wall = "interior_back_wall";
	shader_names->interior_brightness = "interior_brightness";
	shader_names->interior_lights = "interior_lights";
	shader_names->interior_lit_fraction = "interior_lit_fraction";
	shader_names->interior_light_color = "interior_light_color";
	shader_names->interior_curtains = "interior_curtains"; // Mariner G5d.
	shader_names->interior_blinds = "interior_blinds";
	shader_names->interior_curtain_color = "interior_curtain_color";
	shader_names->iridescence = "iridescence";
	shader_names->iridescence_ior = "iridescence_ior";
	shader_names->iridescence_thickness_min = "iridescence_thickness_min";
	shader_names->iridescence_thickness_max = "iridescence_thickness_max";
	shader_names->refraction_ior = "refraction_ior";
	shader_names->refraction_thickness = "refraction_thickness";
	shader_names->refraction_absorption_color = "refraction_absorption_color";
	shader_names->refraction_absorption_distance = "refraction_absorption_distance";

	shader_names->grow = "grow";

	shader_names->ao_light_affect = "ao_light_affect";

	shader_names->proximity_fade_distance = "proximity_fade_distance";
	shader_names->distance_fade_min = "distance_fade_min";
	shader_names->distance_fade_max = "distance_fade_max";

	shader_names->msdf_pixel_range = "msdf_pixel_range";
	shader_names->msdf_outline_size = "msdf_outline_size";

	shader_names->metallic_texture_channel = "metallic_texture_channel";
	shader_names->ao_texture_channel = "ao_texture_channel";
	shader_names->clearcoat_texture_channel = "clearcoat_texture_channel";
	shader_names->rim_texture_channel = "rim_texture_channel";
	shader_names->heightmap_texture_channel = "heightmap_texture_channel";
	shader_names->refraction_texture_channel = "refraction_texture_channel";

	shader_names->transmittance_color = "transmittance_color";
	shader_names->transmittance_depth = "transmittance_depth";
	shader_names->transmittance_boost = "transmittance_boost";

	shader_names->texture_names[TEXTURE_ALBEDO] = "texture_albedo";
	shader_names->texture_names[TEXTURE_METALLIC] = "texture_metallic";
	shader_names->texture_names[TEXTURE_ROUGHNESS] = "texture_roughness";
	shader_names->texture_names[TEXTURE_EMISSION] = "texture_emission";
	shader_names->texture_names[TEXTURE_NORMAL] = "texture_normal";
	shader_names->texture_names[TEXTURE_BENT_NORMAL] = "texture_bent_normal";
	shader_names->texture_names[TEXTURE_SHEEN] = "texture_sheen";
	shader_names->texture_names[TEXTURE_IRIDESCENCE] = "texture_iridescence";
	shader_names->texture_names[TEXTURE_INTERIOR] = "texture_interior"; // Mariner G5a.
	shader_names->texture_names[TEXTURE_INTERIOR_MASK] = "texture_interior_mask";
	shader_names->texture_names[TEXTURE_PLANAR_REFLECTION] = "texture_planar_reflection"; // Mariner G6.
	shader_names->texture_names[TEXTURE_DISPLACEMENT] = "texture_displacement"; // Mariner G7.
	shader_names->texture_names[TEXTURE_ALPHA_MASK] = "texture_alpha_mask"; // Mariner AA2.
	shader_names->texture_names[TEXTURE_RIM] = "texture_rim";
	shader_names->texture_names[TEXTURE_CLEARCOAT] = "texture_clearcoat";
	shader_names->texture_names[TEXTURE_FLOWMAP] = "texture_flowmap";
	shader_names->texture_names[TEXTURE_AMBIENT_OCCLUSION] = "texture_ambient_occlusion";
	shader_names->texture_names[TEXTURE_HEIGHTMAP] = "texture_heightmap";
	shader_names->texture_names[TEXTURE_SUBSURFACE_SCATTERING] = "texture_subsurface_scattering";
	shader_names->texture_names[TEXTURE_SUBSURFACE_TRANSMITTANCE] = "texture_subsurface_transmittance";
	shader_names->texture_names[TEXTURE_BACKLIGHT] = "texture_backlight";
	shader_names->texture_names[TEXTURE_REFRACTION] = "texture_refraction";
	shader_names->texture_names[TEXTURE_DETAIL_MASK] = "texture_detail_mask";
	shader_names->texture_names[TEXTURE_DETAIL_ALBEDO] = "texture_detail_albedo";
	shader_names->texture_names[TEXTURE_DETAIL_NORMAL] = "texture_detail_normal";
	shader_names->texture_names[TEXTURE_ORM] = "texture_orm";

	shader_names->alpha_scissor_threshold = "alpha_scissor_threshold";
	shader_names->alpha_hash_scale = "alpha_hash_scale";

	shader_names->alpha_antialiasing_edge = "alpha_antialiasing_edge";
	shader_names->albedo_texture_size = "albedo_texture_size";
	shader_names->z_clip_scale = "z_clip_scale";
	shader_names->fov_override = "fov_override";
}

HashMap<uint64_t, Ref<StandardMaterial3D>> BaseMaterial3D::materials_for_2d;

void BaseMaterial3D::finish_shaders() {
	materials_for_2d.clear();

	dirty_materials.clear();

	memdelete(shader_names);
	shader_names = nullptr;
}

void BaseMaterial3D::_update_shader() {
	if (!_is_initialized()) {
		_mark_ready();
	}

	MaterialKey mk = _compute_key();
	if (mk == current_key) {
		return; //no update required in the end
	}

	{
		MutexLock lock(shader_map_mutex);
		ShaderData *v = shader_map.getptr(current_key);
		if (v) {
			v->users--;
			if (v->users == 0) {
				// Deallocate shader which is no longer in use.
				shader_rid = RID();
				RS::get_singleton()->free_rid(v->shader);
				shader_map.erase(current_key);
			}
		}

		current_key = mk;

		v = shader_map.getptr(mk);
		if (v) {
			shader_rid = v->shader;
			v->users++;

			if (_get_material().is_valid()) {
				RS::get_singleton()->material_set_shader(_get_material(), shader_rid);
			}

			return;
		}
	}

	// From this point, it is possible that multiple threads requesting the same key will
	// race to create the shader. The winner, which is the one found in shader_map, will be
	// used. The losers will free their shader.

	String texfilter_str;
	// Force linear filtering for the heightmap texture, as the heightmap effect
	// looks broken with nearest-neighbor filtering (with and without Deep Parallax).
	String texfilter_height_str;
	switch (texture_filter) {
		case TEXTURE_FILTER_NEAREST:
			texfilter_str = "filter_nearest";
			texfilter_height_str = "filter_linear";
			break;
		case TEXTURE_FILTER_LINEAR:
			texfilter_str = "filter_linear";
			texfilter_height_str = "filter_linear";
			break;
		case TEXTURE_FILTER_NEAREST_WITH_MIPMAPS:
			texfilter_str = "filter_nearest_mipmap";
			texfilter_height_str = "filter_linear_mipmap";
			break;
		case TEXTURE_FILTER_LINEAR_WITH_MIPMAPS:
			texfilter_str = "filter_linear_mipmap";
			texfilter_height_str = "filter_linear_mipmap";
			break;
		case TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC:
			texfilter_str = "filter_nearest_mipmap_anisotropic";
			texfilter_height_str = "filter_linear_mipmap_anisotropic";
			break;
		case TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC:
			texfilter_str = "filter_linear_mipmap_anisotropic";
			texfilter_height_str = "filter_linear_mipmap_anisotropic";
			break;
		case TEXTURE_FILTER_MAX:
			break; // Internal value, skip.
	}

	// Mariner G5c: the box impostor's visual hull (the faces' data set by BoxImpostorBaker).
	const bool impostor_hull = features[FEATURE_HEIGHT_MAPPING] && !flags[FLAG_UV1_USE_TRIPLANAR] && deep_parallax && heightmap_impostor && heightmap_impostor_faces.size() >= IMPOSTOR_FACE_DATA_SIZE;

	// Mariner G7: the vertices move along their normals by the displacement map (not with the billboards, which replace
	// the model-view matrix, nor with the triplanar mapping, which has no UV for the vertex).
	const bool displacement = features[FEATURE_DISPLACEMENT] && billboard_mode == BILLBOARD_DISABLED && !flags[FLAG_UV1_USE_TRIPLANAR];

	// Mariner G5a: the interior atlas is sampled by room, never repeated.
	const String texfilter_interior_str = texfilter_str + ", repeat_disable";

	if (flags[FLAG_USE_TEXTURE_REPEAT]) {
		texfilter_str += ", repeat_enable";
		texfilter_height_str += ", repeat_enable";
	} else {
		texfilter_str += ", repeat_disable";
		texfilter_height_str += ", repeat_disable";
	}

	// Add a comment to describe the shader origin (useful when converting to ShaderMaterial).
	String code = vformat(
			"// NOTE: Shader automatically converted from " GODOT_VERSION_NAME " " GODOT_VERSION_FULL_CONFIG "'s %s.\n\n",
			orm ? "ORMMaterial3D" : "StandardMaterial3D");

	// Define shader type and render mode based on property values.
	code += "shader_type spatial;\nrender_mode ";
	switch (blend_mode) {
		case BLEND_MODE_MIX:
			code += "blend_mix";
			break;
		case BLEND_MODE_ADD:
			code += "blend_add";
			break;
		case BLEND_MODE_SUB:
			code += "blend_sub";
			break;
		case BLEND_MODE_MUL:
			code += "blend_mul";
			break;
		case BLEND_MODE_PREMULT_ALPHA:
			code += "blend_premul_alpha";
			break;
		case BLEND_MODE_MAX:
			break; // Internal value, skip.
	}

	DepthDrawMode ddm = depth_draw_mode;
	if (features[FEATURE_REFRACTION]) {
		ddm = DEPTH_DRAW_ALWAYS;
	}

	switch (ddm) {
		case DEPTH_DRAW_OPAQUE_ONLY:
			code += ", depth_draw_opaque";
			break;
		case DEPTH_DRAW_ALWAYS:
			code += ", depth_draw_always";
			break;
		case DEPTH_DRAW_DISABLED:
			code += ", depth_draw_never";
			break;
		case DEPTH_DRAW_MAX:
			break; // Internal value, skip.
	}

	switch (cull_mode) {
		case CULL_BACK:
			code += ", cull_back";
			break;
		case CULL_FRONT:
			code += ", cull_front";
			break;
		case CULL_DISABLED:
			code += ", cull_disabled";
			break;
		case CULL_MAX:
			break; // Internal value, skip.
	}
	switch (diffuse_mode) {
		case DIFFUSE_BURLEY:
			code += ", diffuse_burley";
			break;
		case DIFFUSE_LAMBERT:
			code += ", diffuse_lambert";
			break;
		case DIFFUSE_LAMBERT_WRAP:
			code += ", diffuse_lambert_wrap";
			break;
		case DIFFUSE_TOON:
			code += ", diffuse_toon";
			break;
		case DIFFUSE_MAX:
			break; // Internal value, skip.
	}
	switch (specular_mode) {
		case SPECULAR_SCHLICK_GGX:
			code += ", specular_schlick_ggx";
			break;
		case SPECULAR_TOON:
			code += ", specular_toon";
			break;
		case SPECULAR_DISABLED:
			code += ", specular_disabled";
			break;
		case SPECULAR_MAX:
			break; // Internal value, skip.
	}
	if (features[FEATURE_SUBSURFACE_SCATTERING] && flags[FLAG_SUBSURFACE_MODE_SKIN]) {
		code += ", sss_mode_skin";
	}

	if (shading_mode == SHADING_MODE_UNSHADED) {
		code += ", unshaded";
	}
	if (flags[FLAG_DISABLE_DEPTH_TEST]) {
		code += ", depth_test_disabled";
	} else {
		switch (depth_test) {
			case DEPTH_TEST_DEFAULT:
				// depth_test_default is the default behavior, no need to emit it here.
				break;
			case DEPTH_TEST_INVERTED:
				code += ", depth_test_inverted";
				break;
			case DEPTH_TEST_MAX:
				break; // Internal value, skip.
		}
	}
	if (flags[FLAG_PARTICLE_TRAILS_MODE]) {
		code += ", particle_trails";
	}
	if (shading_mode == SHADING_MODE_PER_VERTEX) {
		code += ", vertex_lighting";
	}
	if (flags[FLAG_DONT_RECEIVE_SHADOWS]) {
		code += ", shadows_disabled";
	}
	if (flags[FLAG_DISABLE_AMBIENT_LIGHT]) {
		code += ", ambient_light_disabled";
	}
	if (flags[FLAG_USE_SHADOW_TO_OPACITY]) {
		code += ", shadow_to_opacity";
	}
	if (flags[FLAG_DISABLE_FOG]) {
		code += ", fog_disabled";
	}
	if (flags[FLAG_DISABLE_SPECULAR_OCCLUSION]) {
		code += ", specular_occlusion_disabled";
	}
	if (transparency == TRANSPARENCY_ALPHA_DEPTH_PRE_PASS) {
		code += ", depth_prepass_alpha";
	}

	// Although it's technically possible to do alpha antialiasing without using alpha hash or alpha scissor,
	// it is restricted in the base material because it has no use, and abusing it with regular Alpha blending can
	// saturate the MSAA mask.
	if (transparency == TRANSPARENCY_ALPHA_HASH || transparency == TRANSPARENCY_ALPHA_SCISSOR) {
		// Alpha antialiasing is only useful with ALPHA_HASH or ALPHA_SCISSOR.
		if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE) {
			code += ", alpha_to_coverage";
		} else if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE) {
			code += ", alpha_to_coverage_and_one";
		} else if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_FRINGE && transparency == TRANSPARENCY_ALPHA_SCISSOR) {
			code += ", alpha_fringe";
		}
	}

	code += ";\n";

	if (stencil_mode != STENCIL_MODE_DISABLED && stencil_flags != 0) {
		code += "stencil_mode ";

		if (stencil_flags & STENCIL_FLAG_READ) {
			code += "read";
		}

		if (stencil_flags & STENCIL_FLAG_WRITE) {
			if (stencil_flags & STENCIL_FLAG_READ) {
				code += ", ";
			}
			code += "write";
		}

		if (stencil_flags & STENCIL_FLAG_WRITE_DEPTH_FAIL) {
			if (stencil_flags & (STENCIL_FLAG_READ | STENCIL_FLAG_WRITE)) {
				code += ", ";
			}
			code += "write_depth_fail";
		}

		switch (stencil_compare) {
			case STENCIL_COMPARE_ALWAYS:
				code += ", compare_always";
				break;
			case STENCIL_COMPARE_LESS:
				code += ", compare_less";
				break;
			case STENCIL_COMPARE_EQUAL:
				code += ", compare_equal";
				break;
			case STENCIL_COMPARE_LESS_OR_EQUAL:
				code += ", compare_less_or_equal";
				break;
			case STENCIL_COMPARE_GREATER:
				code += ", compare_greater";
				break;
			case STENCIL_COMPARE_NOT_EQUAL:
				code += ", compare_not_equal";
				break;
			case STENCIL_COMPARE_GREATER_OR_EQUAL:
				code += ", compare_greater_or_equal";
				break;
			case STENCIL_COMPARE_MAX:
				break;
		}

		code += vformat(", %s;\n", stencil_reference);
	}

	// Generate list of uniforms.
	code += vformat(R"(
uniform vec4 albedo : source_color;
uniform sampler2D texture_albedo : source_color, %s;
)",
			texfilter_str);

	if (grow_enabled) {
		code += "uniform float grow : hint_range(-16.0, 16.0, 0.001);\n";
	}

	if (proximity_fade_enabled) {
		code += "uniform float proximity_fade_distance : hint_range(0.0, 4096.0, 0.01);\n";
	}
	if (distance_fade != DISTANCE_FADE_DISABLED) {
		code += R"(
uniform float distance_fade_min : hint_range(0.0, 4096.0, 0.01);
uniform float distance_fade_max : hint_range(0.0, 4096.0, 0.01);
)";
	}

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && flags[FLAG_UV1_USE_TRIPLANAR]) {
		String msg = "MSDF is not supported on triplanar materials. Ignoring MSDF in favor of triplanar mapping.";
		if (textures[TEXTURE_ALBEDO].is_valid()) {
			WARN_PRINT(vformat("%s (albedo %s): " + msg, get_path(), textures[TEXTURE_ALBEDO]->get_path()));
		} else if (!get_path().is_empty()) {
			WARN_PRINT(vformat("%s: " + msg, get_path()));
		} else {
			WARN_PRINT(msg);
		}
	}

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
uniform float msdf_pixel_range : hint_range(1.0, 100.0, 1.0);
uniform float msdf_outline_size : hint_range(0.0, 250.0, 1.0);
)";
	}

	if (alpha_mask_mode != ALPHA_MASK_ALBEDO && transparency != TRANSPARENCY_DISABLED) {
		// Mariner AA2: the alpha mask (a distance field; RenderRaytracing reads this marker for the path tracer).
		code += vformat("uniform sampler2D texture_alpha_mask : hint_default_white, %s; // Mariner AA2: alpha mask %s\n", texfilter_str, alpha_mask_mode == ALPHA_MASK_MSDF ? "MSDF" : "SDF");
	}

	// Alpha scissor is only valid if there is no antialiasing edge.
	// Alpha hash is valid whenever, but not with alpha scissor.
	if (transparency == TRANSPARENCY_ALPHA_SCISSOR) {
		code += "uniform float alpha_scissor_threshold : hint_range(0.0, 1.0, 0.001);\n";
	} else if (transparency == TRANSPARENCY_ALPHA_HASH) {
		code += "uniform float alpha_hash_scale : hint_range(0.0, 2.0, 0.01);\n";
	}
	// If alpha antialiasing isn't off, add in the edge variable.
	if (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF && alpha_antialiasing_mode != ALPHA_ANTIALIASING_FRINGE &&
			(transparency == TRANSPARENCY_ALPHA_SCISSOR || transparency == TRANSPARENCY_ALPHA_HASH)) {
		code += "uniform float alpha_antialiasing_edge : hint_range(0.0, 1.0, 0.01);\n";
	}

	code += "uniform ivec2 albedo_texture_size;\n";
	code += "uniform float point_size : hint_range(0.1, 128.0, 0.1);\n";

	if (!orm) {
		code += vformat(R"(
uniform float roughness : hint_range(0.0, 1.0);
uniform sampler2D texture_metallic : hint_default_white, %s;
uniform vec4 metallic_texture_channel;
)",
				texfilter_str);
		switch (roughness_texture_channel) {
			case TEXTURE_CHANNEL_RED: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_r, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_GREEN: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_g, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_BLUE: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_b, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_ALPHA: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_a, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_GRAYSCALE: {
				code += vformat("uniform sampler2D texture_roughness : hint_roughness_gray, %s;\n", texfilter_str);
			} break;
			case TEXTURE_CHANNEL_MAX:
				break; // Internal value, skip.
		}

		code += R"(
uniform float specular : hint_range(0.0, 1.0, 0.01);
uniform float metallic : hint_range(0.0, 1.0, 0.01);
)";
	} else {
		code += "uniform sampler2D texture_orm : hint_roughness_g, " + texfilter_str + ";\n";
	}

	if (billboard_mode == BILLBOARD_PARTICLES) {
		code += R"(
uniform int particles_anim_h_frames : hint_range(1, 128);
uniform int particles_anim_v_frames : hint_range(1, 128);
uniform bool particles_anim_loop;
)";
	}

	if (features[FEATURE_EMISSION]) {
		code += vformat(R"(
uniform sampler2D texture_emission : source_color, hint_default_black, %s;
uniform vec4 emission : source_color;
uniform float emission_energy : hint_range(0.0, 100.0, 0.01);
)",
				texfilter_str);
	}

	if (features[FEATURE_REFRACTION]) {
		code += vformat(R"(
uniform sampler2D texture_refraction : %s;
uniform float refraction : hint_range(-1.0, 1.0, 0.001);
uniform vec4 refraction_texture_channel;
)",
				texfilter_str);
		if (refraction_physical) {
			code += R"(uniform float refraction_ior : hint_range(1.0, 3.0, 0.01);
uniform float refraction_thickness : hint_range(0.0, 1.0, 0.001);
uniform vec4 refraction_absorption_color : source_color;
uniform float refraction_absorption_distance : hint_range(0.001, 100.0, 0.001);
)";
		}
	}

	if (features[FEATURE_REFRACTION]) {
		code += "uniform sampler2D screen_texture : hint_screen_texture, repeat_disable, filter_linear_mipmap;\n";
	}

	if (features[FEATURE_REFRACTION] || proximity_fade_enabled) {
		code += "uniform sampler2D depth_texture : hint_depth_texture, repeat_disable, filter_nearest;\n";
	}

	if (features[FEATURE_NORMAL_MAPPING]) {
		code += vformat(R"(
uniform sampler2D texture_normal : hint_roughness_normal, %s;
uniform float normal_scale : hint_range(-16.0, 16.0);
)",
				texfilter_str);
	}
	if (features[FEATURE_BENT_NORMAL_MAPPING]) {
		code += vformat(R"(
uniform sampler2D texture_bent_normal : hint_roughness_normal, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_RIM]) {
		code += vformat(R"(
uniform float rim : hint_range(0.0, 1.0, 0.01);
uniform float rim_tint : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_rim : hint_default_white, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_CLEARCOAT]) {
		code += vformat(R"(
uniform float clearcoat : hint_range(0.0, 1.0, 0.01);
uniform float clearcoat_roughness : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_clearcoat : hint_default_white, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_WETNESS]) {
		code += R"(
uniform float wetness_amount : hint_range(0.0, 1.0, 0.01);
uniform float wetness_porosity : hint_range(0.0, 1.0, 0.01);
uniform float wetness_puddles : hint_range(0.0, 1.0, 0.01);
)";
	}
	if (features[FEATURE_SNOW]) {
		code += R"(
uniform float snow_amount : hint_range(0.0, 1.0, 0.01);
uniform vec4 snow_color : source_color;
uniform float snow_roughness : hint_range(0.0, 1.0, 0.01);
uniform float snow_max_slope : hint_range(0.0, 90.0, 0.1);
uniform float snow_relief : hint_range(0.0, 1.0, 0.01);
)";
	}
	if (features[FEATURE_PLANAR_REFLECTION] && shading_mode != SHADING_MODE_UNSHADED) {
		// Mariner G6: the scene's light (linear HDR) seen by the mirrored camera of a PlanarReflection3D.
		code += R"(
uniform sampler2D texture_planar_reflection : hint_default_black, filter_linear, repeat_disable;
uniform float planar_reflection_amount : hint_range(0.0, 1.0, 0.01);
uniform float planar_reflection_distortion : hint_range(0.0, 0.2, 0.001);
)";
	}
	if (displacement) {
		// Mariner G7: read in the vertex shader at a fixed mipmap (no screen derivatives there).
		code += vformat(R"(
uniform sampler2D texture_displacement : hint_default_black, filter_linear_mipmap, %s;
uniform float displacement_scale;
uniform float displacement_midlevel : hint_range(0.0, 1.0, 0.01);
uniform float displacement_texture_lod : hint_range(0.0, 12.0, 0.1);
varying vec3 displacement_obj;
varying vec3 displacement_obj_normal;
)",
				flags[FLAG_USE_TEXTURE_REPEAT] ? "repeat_enable" : "repeat_disable");
	}
	if (features[FEATURE_INTERIOR]) {
		code += vformat(R"(
uniform sampler2D texture_interior : source_color, hint_default_black, %s;
uniform sampler2D texture_interior_mask : hint_default_white, %s;
uniform vec2 interior_rooms;
uniform vec3 interior_room_size;
uniform ivec2 interior_atlas_size;
uniform float interior_back_wall : hint_range(0.05, 0.95, 0.01);
uniform float interior_brightness : hint_range(0.0, 16.0, 0.01);
uniform float interior_lights : hint_range(0.0, 16.0, 0.01);
uniform float interior_lit_fraction : hint_range(0.0, 1.0, 0.01);
uniform vec4 interior_light_color : source_color;
uniform float interior_curtains : hint_range(0.0, 1.0, 0.01);
uniform float interior_blinds : hint_range(0.0, 1.0, 0.01);
uniform vec4 interior_curtain_color : source_color;
)",
				texfilter_interior_str, texfilter_str);
	}
	if (features[FEATURE_SHEEN]) {
		code += vformat(R"(
uniform vec4 sheen_color : source_color;
uniform float sheen_roughness : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_sheen : source_color, hint_default_white, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_IRIDESCENCE]) {
		code += vformat(R"(
uniform float iridescence : hint_range(0.0, 1.0, 0.01);
uniform float iridescence_ior : hint_range(1.0, 2.333, 0.01);
uniform float iridescence_thickness_min : hint_range(0.0, 1200.0, 1.0);
uniform float iridescence_thickness_max : hint_range(0.0, 1200.0, 1.0);
uniform sampler2D texture_iridescence : hint_default_white, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_ANISOTROPY]) {
		code += vformat(R"(
uniform float anisotropy_ratio : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_flowmap : hint_anisotropy, %s;
)",
				texfilter_str);
	}
	if (features[FEATURE_AMBIENT_OCCLUSION]) {
		code += vformat(R"(
uniform sampler2D texture_ambient_occlusion : hint_default_white, %s;
uniform vec4 ao_texture_channel;
uniform float ao_light_affect : hint_range(0.0, 1.0, 0.01);
)",
				texfilter_str);
	}

	if (features[FEATURE_DETAIL]) {
		code += vformat(R"(
uniform sampler2D texture_detail_albedo : source_color, %s;
uniform sampler2D texture_detail_normal : hint_normal, %s;
uniform sampler2D texture_detail_mask : hint_default_white, %s;
)",
				texfilter_str, texfilter_str, texfilter_str);
	}

	if (features[FEATURE_SUBSURFACE_SCATTERING]) {
		code += vformat(R"(
uniform float subsurface_scattering_strength : hint_range(0.0, 1.0, 0.01);
uniform sampler2D texture_subsurface_scattering : hint_default_white, %s;
)",
				texfilter_str);
	}

	if (features[FEATURE_SUBSURFACE_TRANSMITTANCE]) {
		code += vformat(R"(
uniform vec4 transmittance_color : source_color;
uniform float transmittance_depth : hint_range(0.001, 8.0, 0.001);
uniform sampler2D texture_subsurface_transmittance : hint_default_white, %s;
uniform float transmittance_boost : hint_range(0.0, 1.0, 0.01);
)",
				texfilter_str);
	}

	if (features[FEATURE_BACKLIGHT]) {
		code += vformat(R"(
uniform vec4 backlight : source_color;
uniform sampler2D texture_backlight : hint_default_black, %s;
)",
				texfilter_str);
	}

	if (features[FEATURE_HEIGHT_MAPPING]) {
		code += vformat(R"(
uniform sampler2D texture_heightmap : hint_default_black, %s;
uniform float heightmap_scale : hint_range(-16.0, 16.0, 0.001);
uniform int heightmap_min_layers : hint_range(1, 64);
uniform int heightmap_max_layers : hint_range(1, 64);
uniform vec2 heightmap_flip;
)",
				(deep_parallax && heightmap_impostor) ? String("filter_nearest_mipmap, repeat_disable") : texfilter_height_str); // Mariner G5b: sharp cliffs at the silhouettes.
		if (deep_parallax && heightmap_impostor) {
			code += "varying vec4 impostor_rect; // Mariner G5b: the face's rectangle in the atlas (CUSTOM0).\n";
		}
		if (impostor_hull) {
			code += vformat(R"(uniform vec4 impostor_faces[%d]; // Mariner G5c.
varying vec3 impostor_obj;
varying vec3 impostor_obj_eye;
varying vec3 impostor_obj_axis;
)",
					IMPOSTOR_FACE_DATA_SIZE);
		}
		if (heightmap_self_shadow) {
			code += "uniform float heightmap_self_shadow_strength : hint_range(0.0, 1.0, 0.01);\n";
		}
	}
	if (flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += "varying vec3 uv1_triplanar_pos;\n";
	}
	if (flags[FLAG_UV2_USE_TRIPLANAR]) {
		code += "varying vec3 uv2_triplanar_pos;\n";
	}
	if (flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
uniform float uv1_blend_sharpness : hint_range(0.0, 150.0, 0.001);
varying vec3 uv1_power_normal;
)";
	}

	if (flags[FLAG_UV2_USE_TRIPLANAR]) {
		code += R"(uniform float uv2_blend_sharpness : hint_range(0.0, 150.0, 0.001);
varying vec3 uv2_power_normal;
)";
	}

	code += R"(
uniform vec3 uv1_scale;
uniform vec3 uv1_offset;
uniform vec3 uv2_scale;
uniform vec3 uv2_offset;
)";

	if (flags[FLAG_USE_Z_CLIP_SCALE]) {
		code += "uniform float z_clip_scale : hint_range(0.01, 1.0, 0.01);\n";
	}

	if (flags[FLAG_USE_FOV_OVERRIDE]) {
		code += "uniform float fov_override : hint_range(1.0, 179.0, 0.1);\n";
	}

	const bool wire_aa = wire_antialiasing && transparency == TRANSPARENCY_ALPHA; // Mariner AA3.
	if (wire_aa) {
		code += R"(
// Mariner AA3: wire antialiasing (Persson) - widened to at least a pixel, faded by the true width.
varying float wire_fade;
varying float wire_r_px;
varying float wire_u_px;
)";
	}

	// Generate vertex shader.
	code += R"(
void vertex() {)";

	if (features[FEATURE_HEIGHT_MAPPING] && !flags[FLAG_UV1_USE_TRIPLANAR] && deep_parallax && heightmap_impostor) {
		code += "\n	impostor_rect = CUSTOM0; // Mariner G5b.\n";
	}
	if (impostor_hull) {
		// Mariner G5c: the ray in object space - from the camera (or along its axis for an orthographic one).
		code += R"(	impostor_obj = VERTEX;
	mat4 impostor_inv_model = inverse(MODEL_MATRIX);
	impostor_obj_eye = (impostor_inv_model * vec4(CAMERA_POSITION_WORLD, 1.0)).xyz;
	impostor_obj_axis = (impostor_inv_model * vec4(-INV_VIEW_MATRIX[2].xyz, 0.0)).xyz;
)";
	}

	if (flags[FLAG_SRGB_VERTEX_COLOR]) {
		code += R"(
	// Vertex Color is sRGB: Enabled
	if (!OUTPUT_IS_SRGB) {
		COLOR.rgb = mix(
				pow((COLOR.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)),
				COLOR.rgb * (1.0 / 12.92),
				lessThan(COLOR.rgb, vec3(0.04045)));
	}
)";
	}
	if (flags[FLAG_USE_POINT_SIZE]) {
		code += R"(
	// Use Point Size: Enabled
	POINT_SIZE = point_size;
)";
	}

	if (shading_mode == SHADING_MODE_PER_VERTEX) {
		code += R"(
	// Shading Mode: Per Vertex
	ROUGHNESS = roughness;
)";
	}

	if (!flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
	UV = UV * uv1_scale.xy + uv1_offset.xy;
)";
	}

	if (detail_uv == DETAIL_UV_2 && !flags[FLAG_UV2_USE_TRIPLANAR]) {
		// Don't add a newline if the UV assignment above is already performed,
		// so that UV1 and UV2 are closer to each other.
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "\n";
		}
		code += R"(	// Detail UV Layer: UV2
	UV2 = UV2 * uv2_scale.xy + uv2_offset.xy;
)";
	}

	switch (billboard_mode) {
		case BILLBOARD_DISABLED: {
		} break;
		case BILLBOARD_ENABLED: {
			// `MAIN_CAM_INV_VIEW_MATRIX` is inverse of the camera, even on shadow passes.
			// This ensures the billboard faces the camera when casting shadows.
			code += R"(
	// Billboard Mode: Enabled
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(
			MAIN_CAM_INV_VIEW_MATRIX[0],
			MAIN_CAM_INV_VIEW_MATRIX[1],
			MAIN_CAM_INV_VIEW_MATRIX[2],
			MODEL_MATRIX[3]);
)";
			if (flags[FLAG_BILLBOARD_KEEP_SCALE]) {
				code += R"(
	// Billboard Keep Scale: Enabled
	MODELVIEW_MATRIX = MODELVIEW_MATRIX * mat4(
			vec4(length(MODEL_MATRIX[0].xyz), 0.0, 0.0, 0.0),
			vec4(0.0, length(MODEL_MATRIX[1].xyz), 0.0, 0.0),
			vec4(0.0, 0.0, length(MODEL_MATRIX[2].xyz), 0.0),
			vec4(0.0, 0.0, 0.0, 1.0));
)";
			}
			code += "	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);\n";
		} break;
		case BILLBOARD_FIXED_Y: {
			// `MAIN_CAM_INV_VIEW_MATRIX` is inverse of the camera, even on shadow passes.
			// This ensures the billboard faces the camera when casting shadows.
			code += R"(
	// Billboard Mode: Y-Billboard
	MODELVIEW_MATRIX = VIEW_MATRIX * mat4(
			vec4(normalize(cross(vec3(0.0, 1.0, 0.0), MAIN_CAM_INV_VIEW_MATRIX[2].xyz)), 0.0),
			vec4(0.0, 1.0, 0.0, 0.0),
			vec4(normalize(cross(MAIN_CAM_INV_VIEW_MATRIX[0].xyz, vec3(0.0, 1.0, 0.0))), 0.0),
			MODEL_MATRIX[3]);
)";
			if (flags[FLAG_BILLBOARD_KEEP_SCALE]) {
				code += R"(
	// Billboard Keep Scale: Enabled
	MODELVIEW_MATRIX = MODELVIEW_MATRIX * mat4(
			vec4(length(MODEL_MATRIX[0].xyz), 0.0, 0.0, 0.0),
			vec4(0.0, length(MODEL_MATRIX[1].xyz), 0.0, 0.0),
			vec4(0.0, 0.0, length(MODEL_MATRIX[2].xyz), 0.0),
			vec4(0.0, 0.0, 0.0, 1.0));
)";
			}
			code += "	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);\n";
		} break;
		case BILLBOARD_PARTICLES: {
			// Make billboard and rotated by rotation.
			code += R"(
	// Billboard Mode: Particles
	mat4 mat_world = mat4(
			normalize(INV_VIEW_MATRIX[0]),
			normalize(INV_VIEW_MATRIX[1]),
			normalize(INV_VIEW_MATRIX[2]),
			MODEL_MATRIX[3]);
	mat_world = mat_world * mat4(
			vec4(cos(INSTANCE_CUSTOM.x), -sin(INSTANCE_CUSTOM.x), 0.0, 0.0),
			vec4(sin(INSTANCE_CUSTOM.x), cos(INSTANCE_CUSTOM.x), 0.0, 0.0),
			vec4(0.0, 0.0, 1.0, 0.0),
			vec4(0.0, 0.0, 0.0, 1.0));
)";
			// Set modelview.
			code += "	MODELVIEW_MATRIX = VIEW_MATRIX * mat_world;\n";
			if (flags[FLAG_BILLBOARD_KEEP_SCALE]) {
				code += R"(
	// Billboard Keep Scale: Enabled
	MODELVIEW_MATRIX = MODELVIEW_MATRIX * mat4(
			vec4(length(MODEL_MATRIX[0].xyz), 0.0, 0.0, 0.0),
			vec4(0.0, length(MODEL_MATRIX[1].xyz), 0.0, 0.0),
			vec4(0.0, 0.0, length(MODEL_MATRIX[2].xyz), 0.0),
			vec4(0.0, 0.0, 0.0, 1.0));
)";
			}
			// Set modelview normal and handle animation.
			code += R"(
	MODELVIEW_NORMAL_MATRIX = mat3(MODELVIEW_MATRIX);

	float h_frames = float(particles_anim_h_frames);
	float v_frames = float(particles_anim_v_frames);
	float particle_total_frames = float(particles_anim_h_frames * particles_anim_v_frames);
	float particle_frame = floor(INSTANCE_CUSTOM.z * float(particle_total_frames));
	if (!particles_anim_loop) {
		particle_frame = clamp(particle_frame, 0.0, particle_total_frames - 1.0);
	} else {
		particle_frame = mod(particle_frame, particle_total_frames);
	}
	UV /= vec2(h_frames, v_frames);
	UV += vec2(mod(particle_frame, h_frames) / h_frames, floor((particle_frame + 0.5) / h_frames) / v_frames);
)";
		} break;
		case BILLBOARD_MAX:
			break; // Internal value, skip.
	}

	if (flags[FLAG_FIXED_SIZE]) {
		code += R"(
	// Fixed Size: Enabled
	if (PROJECTION_MATRIX[3][3] != 0.0) {
		// Orthogonal matrix; try to do about the same with viewport size.
		float h = abs(1.0 / (2.0 * PROJECTION_MATRIX[1][1]));
		// Consistent with vertical FOV (Keep Height).
		float sc = (h * 2.0);
		MODELVIEW_MATRIX[0] *= sc;
		MODELVIEW_MATRIX[1] *= sc;
		MODELVIEW_MATRIX[2] *= sc;
	} else {
		// Scale by depth.
		float sc;
		if (IS_MULTIVIEW) {
			// Assuming stereo rendering.
			// Moving in the z-plane gives the illusion of the object growing/shrinking in size.
			// We need to take the full distance to camera to compensate.
			sc = length((MODELVIEW_MATRIX)[3].xyz);
		} else {
			sc = -(MODELVIEW_MATRIX)[3].z;
		}
		MODELVIEW_MATRIX[0] *= sc;
		MODELVIEW_MATRIX[1] *= sc;
		MODELVIEW_MATRIX[2] *= sc;
	}
)";
	}

	if (flags[FLAG_UV1_USE_TRIPLANAR] || flags[FLAG_UV2_USE_TRIPLANAR]) {
		// Generate tangent and binormal in world space.
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += R"(
	vec3 normal = MODEL_NORMAL_MATRIX * NORMAL;
)";
		} else {
			code += R"(
	vec3 normal = NORMAL;
)";
		}
		code += R"(
	TANGENT = vec3(0.0, 0.0, -1.0) * abs(normal.x);
	TANGENT += vec3(1.0, 0.0, 0.0) * abs(normal.y);
	TANGENT += vec3(1.0, 0.0, 0.0) * abs(normal.z);
)";
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += "	TANGENT = inverse(MODEL_NORMAL_MATRIX) * normalize(TANGENT);\n";
		} else {
			code += "	TANGENT = normalize(TANGENT);\n";
		}

		code += R"(
	BINORMAL = vec3(0.0, 1.0, 0.0) * abs(normal.x);
	BINORMAL += vec3(0.0, 0.0, -1.0) * abs(normal.y);
	BINORMAL += vec3(0.0, 1.0, 0.0) * abs(normal.z);
)";
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += "	BINORMAL = inverse(MODEL_NORMAL_MATRIX) * normalize(BINORMAL);\n";
		} else {
			code += "	BINORMAL = normalize(BINORMAL);\n";
		}
	}

	if (flags[FLAG_UV1_USE_TRIPLANAR]) {
		if (flags[FLAG_UV1_USE_WORLD_TRIPLANAR]) {
			code += R"(
	// UV1 Triplanar: Enabled (with World Triplanar)
	uv1_power_normal = pow(abs(normal), vec3(uv1_blend_sharpness));
	uv1_triplanar_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz * uv1_scale + uv1_offset;
)";
		} else {
			code += R"(
	// UV1 Triplanar: Enabled
	uv1_power_normal = pow(abs(NORMAL), vec3(uv1_blend_sharpness));
	uv1_triplanar_pos = VERTEX * uv1_scale + uv1_offset;
)";
		}
		code += R"(	uv1_power_normal /= dot(uv1_power_normal, vec3(1.0));
	uv1_triplanar_pos *= vec3(1.0, -1.0, 1.0);
)";
	}

	if (flags[FLAG_UV2_USE_TRIPLANAR]) {
		if (flags[FLAG_UV2_USE_WORLD_TRIPLANAR]) {
			code += R"(
	// UV2 Triplanar: Enabled (with World Triplanar)
	uv2_power_normal = pow(abs(mat3(MODEL_MATRIX) * NORMAL), vec3(uv2_blend_sharpness));
	uv2_triplanar_pos = (MODEL_MATRIX * vec4(VERTEX, 1.0)).xyz * uv2_scale + uv2_offset;
)";
		} else {
			code += R"(
	// UV2 Triplanar: Enabled
	uv2_power_normal = pow(abs(NORMAL), vec3(uv2_blend_sharpness));
	uv2_triplanar_pos = VERTEX * uv2_scale + uv2_offset;
)";
		}
		code += R"(	uv2_power_normal /= dot(uv2_power_normal, vec3(1.0));
	uv2_triplanar_pos *= vec3(1.0, -1.0, 1.0);
)";
	}

	if (displacement) {
		// Mariner G7: the undisplaced point and normal go to the fragment shader, which builds the relief's normal.
		code += R"(
	// Displacement: Enabled
	displacement_obj = VERTEX;
	displacement_obj_normal = NORMAL;
	VERTEX += normalize(NORMAL) * ((textureLod(texture_displacement, UV, displacement_texture_lod).r - displacement_midlevel) * displacement_scale);
)";
	}

	if (grow_enabled) {
		code += R"(
	// Grow: Enabled
	VERTEX += NORMAL * grow;
)";
	}

	if (wire_aa) {
		// Mariner AA3: the tube's axis is the local Y axis, its radius the vertex's distance from it (tapers work). The
		// tube is widened to at least a pixel plus 0.75 pixel for the edge, its alpha faded by the true width; the
		// edge is the screen distance from the axis (u, along the direction perpendicular to the axis and the view:
		// linear in the position, so exact once interpolated). Shadows keep the true tube.
		code += R"(
	// Wire Antialiasing: Enabled
	wire_fade = 1.0;
	wire_r_px = 1.0e6;
	wire_u_px = 0.0;
	float wire_r = length(VERTEX.xz);
	if (!IN_SHADOW_PASS && wire_r > 1.0e-7) {
		vec3 wire_radial = vec3(VERTEX.x, 0.0, VERTEX.z) / wire_r;
		vec3 wire_c = vec3(0.0, VERTEX.y, 0.0);
		vec4 wire_vc = MODELVIEW_MATRIX * vec4(wire_c, 1.0);
		float wire_scale = length(MODELVIEW_MATRIX[0].xyz);
		bool wire_ortho = PROJECTION_MATRIX[3][3] != 0.0;
		float wire_px = 2.0 * (wire_ortho ? 1.0 : -wire_vc.z) / (abs(PROJECTION_MATRIX[1][1]) * VIEWPORT_SIZE.y); // World size of a pixel.
		float wire_rp = wire_r * wire_scale / wire_px;
		wire_r_px = max(wire_rp, 0.5);
		wire_fade = wire_rp / wire_r_px;
		float wire_geo_px = wire_r_px + 0.75;
		mat4 wire_inv_model = inverse(MODEL_MATRIX);
		vec3 wire_eye = wire_ortho ? -(wire_inv_model * vec4(INV_VIEW_MATRIX[2].xyz, 0.0)).xyz * 1.0e6 : (wire_inv_model * vec4(CAMERA_POSITION_WORLD, 1.0)).xyz;
		vec3 wire_perp = normalize(cross(vec3(0.0, 1.0, 0.0), wire_eye - wire_c));
		wire_u_px = dot(wire_radial, wire_perp) * wire_geo_px;
		VERTEX = wire_c + wire_radial * (wire_geo_px * wire_px / wire_scale);
	}
)";
	}

	if (flags[FLAG_USE_Z_CLIP_SCALE]) {
		code += R"(
	Z_CLIP_SCALE = z_clip_scale;
)";
	}

	if (flags[FLAG_USE_FOV_OVERRIDE]) {
		code += R"(
	if (!IN_SHADOW_PASS) {
		float flip_y = sign(PROJECTION_MATRIX[1][1]);
		float aspect = PROJECTION_MATRIX[1][1] / PROJECTION_MATRIX[0][0];
		float f = flip_y / tan(fov_override * PI / 360.0);
		PROJECTION_MATRIX[0][0] = f / aspect;
		PROJECTION_MATRIX[1][1] = f;
	}
)";
	}

	// End of the vertex shader function.
	code += "}\n";

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
float msdf_median(float r, float g, float b) {
	return max(min(r, g), min(max(r, g), b));
}
)";
	}

	if (flags[FLAG_UV1_USE_TRIPLANAR] || flags[FLAG_UV2_USE_TRIPLANAR]) {
		code += R"(
vec4 triplanar_texture(sampler2D p_sampler, vec3 p_weights, vec3 p_triplanar_pos) {
	vec4 samp = vec4(0.0);
	samp += texture(p_sampler, p_triplanar_pos.xy) * p_weights.z;
	samp += texture(p_sampler, p_triplanar_pos.xz) * p_weights.y;
	samp += texture(p_sampler, p_triplanar_pos.zy * vec2(-1.0, 1.0)) * p_weights.x;
	return samp;
}
)";
	}

	if (impostor_hull) {
		// Mariner G5c: a point is solid where it lies behind the surface every baked face sees (the visual hull of the
		// faces' reliefs); its face is the one that sees it on its surface (facing the ray).
		code += R"(
vec2 impostor_face_uv(int p_face, vec3 p_point) {
	return vec2(dot(p_point, impostor_faces[p_face * 4].xyz) + impostor_faces[p_face * 4].w, dot(p_point, impostor_faces[p_face * 4 + 1].xyz) + impostor_faces[p_face * 4 + 1].w);
}

float impostor_face_error(int p_face, vec3 p_point, vec2 p_dx, vec2 p_dy) {
	float depth = dot(p_point, impostor_faces[p_face * 4 + 2].xyz) + impostor_faces[p_face * 4 + 2].w;
)";
		code += flags[FLAG_INVERT_HEIGHTMAP] ? "	float surface = textureGrad(texture_heightmap, impostor_face_uv(p_face, p_point), p_dx, p_dy).r;\n" : "	float surface = 1.0 - textureGrad(texture_heightmap, impostor_face_uv(p_face, p_point), p_dx, p_dy).r;\n";
		code += R"(	return depth - surface;
}

bool impostor_solid(vec3 p_point, vec2 p_dx, vec2 p_dy) {
	for (int f = 0; f < 6; f++) {
		if (impostor_faces[f * 4 + 3].w != 0.0 && impostor_face_error(f, p_point, p_dx, p_dy) < 0.0) {
			return false;
		}
	}
	return true;
}

int impostor_seeing_face(vec3 p_point, vec3 p_dir, vec2 p_dx, vec2 p_dy) {
	int best = 0;
	float best_score = 1e20;
	for (int f = 0; f < 6; f++) {
		if (impostor_faces[f * 4 + 3].w == 0.0) {
			continue;
		}
		// The face's outward normal is -impostor_faces[f * 4 + 2].xyz (the depth grows inward): a face the ray enters.
		float facing = dot(-impostor_faces[f * 4 + 2].xyz, p_dir) < 0.0 ? 0.0 : 1.0;
		float score = abs(impostor_face_error(f, p_point, p_dx, p_dy)) + facing;
		if (score < best_score) {
			best_score = score;
			best = f;
		}
	}
	return best;
}
)";
	}

	// Generate fragment shader.
	code += R"(
void fragment() {)";

	if (!flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
	vec2 base_uv = UV;
)";
	}

	if ((features[FEATURE_DETAIL] && detail_uv == DETAIL_UV_2) || (features[FEATURE_AMBIENT_OCCLUSION] && flags[FLAG_AO_ON_UV2]) || (features[FEATURE_EMISSION] && flags[FLAG_EMISSION_ON_UV2])) {
		// Don't add a newline if the UV assignment above is already performed,
		// so that UV1 and UV2 are closer to each other.
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "\n";
		}
		code += R"(	vec2 base_uv2 = UV2;
)";
	}

	if (displacement && displacement_normal) {
		// Mariner G7: the normal of the displaced surface, per pixel - the surface gradient of the height (Mikkelsen,
		// "Bump Mapping Unparametrized Surfaces on the GPU") on the undisplaced surface, in object space. The height's
		// slope is read over a texel or the pixel's footprint, whichever is larger; a coarse mesh still shades its relief.
		code += R"(
	// Displacement Normal: Enabled
	{
		vec3 disp_n = normalize(displacement_obj_normal);
		vec3 disp_dp1 = dFdx(displacement_obj);
		vec3 disp_dp2 = dFdy(displacement_obj);
		vec2 disp_duv1 = dFdx(base_uv);
		vec2 disp_duv2 = dFdy(base_uv);
		vec3 disp_r1 = cross(disp_dp2, disp_n);
		vec3 disp_r2 = cross(disp_n, disp_dp1);
		float disp_det = dot(disp_dp1, disp_r1);
		vec2 disp_step = max(1.0 / vec2(textureSize(texture_displacement, 0)), max(abs(disp_duv1), abs(disp_duv2)));
		vec2 disp_slope = vec2(
				texture(texture_displacement, base_uv + vec2(disp_step.x, 0.0)).r - texture(texture_displacement, base_uv - vec2(disp_step.x, 0.0)).r,
				texture(texture_displacement, base_uv + vec2(0.0, disp_step.y)).r - texture(texture_displacement, base_uv - vec2(0.0, disp_step.y)).r) *
				(displacement_scale * 0.5) / disp_step;
		if (abs(disp_det) > 1e-24) {
			// The gradients of U and V on the surface, times the height's slope along them.
			vec3 disp_grad = (disp_slope.x * (disp_duv1.x * disp_r1 + disp_duv2.x * disp_r2) + disp_slope.y * (disp_duv1.y * disp_r1 + disp_duv2.y * disp_r2)) / disp_det;
			vec3 disp_view_n = normalize(mat3(VIEW_MATRIX) * (MODEL_NORMAL_MATRIX * normalize(disp_n - disp_grad)));
			// The side the renderer shades (the back faces of a double-sided material see the normal turned).
			NORMAL = dot(disp_view_n, NORMAL) < 0.0 ? -disp_view_n : disp_view_n;
		}
	}
)";
	}

	if (features[FEATURE_HEIGHT_MAPPING] && flags[FLAG_UV1_USE_TRIPLANAR]) {
		// Display both resource name and albedo texture name.
		// Materials are often built-in to scenes, so displaying the resource name alone may not be meaningful.
		// On the other hand, albedo textures are almost always external to the scene.
		if (textures[TEXTURE_ALBEDO].is_valid()) {
			WARN_PRINT(vformat("%s (albedo %s): Height mapping is not supported on triplanar materials. Ignoring height mapping in favor of triplanar mapping.", get_path(), textures[TEXTURE_ALBEDO]->get_path()));
		} else if (!get_path().is_empty()) {
			WARN_PRINT(vformat("%s: Height mapping is not supported on triplanar materials. Ignoring height mapping in favor of triplanar mapping.", get_path()));
		} else {
			// Resource wasn't saved yet.
			WARN_PRINT("Height mapping is not supported on triplanar materials. Ignoring height mapping in favor of triplanar mapping.");
		}
	}

	// Mariner G5b: in impostor mode the textures are read with the gradients of the face's UV, not of the parallax
	// offset UV, which jumps across the atlas at the silhouettes (the GPU would take the smallest mipmaps there).
	const bool impostor_uv = features[FEATURE_HEIGHT_MAPPING] && !flags[FLAG_UV1_USE_TRIPLANAR] && deep_parallax && heightmap_impostor;
	const int64_t height_code_start = code.length();
	if (impostor_uv) {
		code += "\n	vec2 impostor_dx = dFdx(base_uv);\n	vec2 impostor_dy = dFdy(base_uv);\n	bool impostor_miss = false;\n";
	}

	if (impostor_hull) {
		// Mariner G5c: the view ray traced through the box in object space against all the faces' reliefs at once
		// (steps over the ray's length in the box, then a binary search); the color, normal and depth of the face that
		// sees the hit.
		code += R"(
	vec3 impostor_normal_obj = vec3(0.0, 1.0, 0.0);
	{
		// Height: Box impostor, visual hull (Mariner G5c)
		vec3 hull_dir = normalize(PROJECTION_MATRIX[3][3] == 1.0 ? impostor_obj_axis : impostor_obj - impostor_obj_eye);
		hull_dir = vec3(abs(hull_dir.x) < 1e-5 ? 1e-5 : hull_dir.x, abs(hull_dir.y) < 1e-5 ? 1e-5 : hull_dir.y, abs(hull_dir.z) < 1e-5 ? 1e-5 : hull_dir.z);
		vec3 hull_far = mix(impostor_faces[24].xyz, impostor_faces[25].xyz, step(0.0, hull_dir));
		vec3 hull_t3 = (hull_far - impostor_obj) / hull_dir;
		float hull_exit = max(min(min(hull_t3.x, hull_t3.y), hull_t3.z), 0.0);
		int hull_steps = max(heightmap_max_layers, 1);
		float hull_dt = hull_exit / float(hull_steps);
		float hull_t0 = 0.0;
		float hull_t1 = 0.0;
		bool hull_hit = false;
		for (int i = 0; i <= hull_steps; i++) {
			hull_t1 = float(i) * hull_dt;
			if (impostor_solid(impostor_obj + hull_dir * hull_t1, impostor_dx, impostor_dy)) {
				hull_hit = true;
				break;
			}
			hull_t0 = hull_t1;
		}
		for (int i = 0; i < 6 && hull_hit; i++) {
			float hull_mid = (hull_t0 + hull_t1) * 0.5;
			if (impostor_solid(impostor_obj + hull_dir * hull_mid, impostor_dx, impostor_dy)) {
				hull_t1 = hull_mid;
			} else {
				hull_t0 = hull_mid;
			}
		}
		vec3 hull_point = impostor_obj + hull_dir * hull_t1;
		int hull_face = impostor_seeing_face(hull_point, hull_dir, impostor_dx, impostor_dy);
		base_uv = impostor_face_uv(hull_face, hull_point);
		impostor_miss = !hull_hit;
)";
		if (features[FEATURE_NORMAL_MAPPING]) {
			code += R"(		// The normal map in the seeing face's tangent frame, to object space.
		vec2 hull_nm = textureGrad(texture_normal, base_uv, impostor_dx, impostor_dy).rg * 2.0 - 1.0;
		vec3 hull_n = normalize(-impostor_faces[hull_face * 4 + 2].xyz);
		vec3 hull_t = impostor_faces[hull_face * 4 + 3].xyz;
		vec3 hull_b = normalize(cross(hull_n, hull_t)) * impostor_faces[hull_face * 4 + 3].w;
		impostor_normal_obj = normalize(hull_t * hull_nm.x + hull_b * hull_nm.y + hull_n * sqrt(max(0.0, 1.0 - dot(hull_nm, hull_nm))));
)";
		} else {
			code += "		impostor_normal_obj = normalize(-impostor_faces[hull_face * 4 + 2].xyz);\n";
		}
		if (heightmap_depth_offset) {
			code += R"(		// The hit's depth (intersections, the sun's shadow maps) and the position its light is computed at.
		vec3 hull_view = (VIEW_MATRIX * (MODEL_MATRIX * vec4(hull_point, 1.0))).xyz;
		LIGHT_VERTEX = hull_view;
		vec4 hull_clip = PROJECTION_MATRIX * vec4(hull_view, 1.0);
		DEPTH = hull_clip.z / hull_clip.w;
)";
			if (OS::get_singleton()->get_current_rendering_method() == "gl_compatibility") {
				code += "		DEPTH = DEPTH * 0.5 + 0.5; // Mariner G5e: OpenGL's clip space depth is -1 .. 1.\n";
			}
		}
		code += "	}\n";
	} else if (features[FEATURE_HEIGHT_MAPPING] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		// Heightmapping isn't supported at the same time as triplanar mapping.
		// Binormal is negative due to mikktspace. Flipping it "unflips" it.
		code += R"(
	{
		// Height: Enabled
		vec3 view_dir = normalize(normalize(-VERTEX + EYE_OFFSET) * mat3(TANGENT * heightmap_flip.x, -BINORMAL * heightmap_flip.y, NORMAL));
		vec2 height_base_uv = base_uv; // Mariner E16: before the parallax offset.
)";
		const bool impostor = deep_parallax && heightmap_impostor;
		if (impostor) {
			// Mariner G5b: under an orthographic projection (the sun's shadow maps) every view ray is the camera's axis.
			code += R"(		vec3 impostor_eye = PROJECTION_MATRIX[3][3] == 1.0 ? vec3(0.0, 0.0, 1.0) : normalize(-VERTEX + EYE_OFFSET);
		view_dir = normalize(impostor_eye * mat3(TANGENT * heightmap_flip.x, -BINORMAL * heightmap_flip.y, NORMAL));
)";
		}

		if (deep_parallax) {
			// Multiply the heightmap scale by 0.01 to improve heightmap scale usability.
			code += R"(
		// Height Deep Parallax: Enabled
		float num_layers = mix(float(heightmap_max_layers), float(heightmap_min_layers), abs(dot(vec3(0.0, 0.0, 1.0), view_dir)));
		float layer_depth = 1.0 / num_layers;
		float current_layer_depth = 0.0;
		vec2 p = view_dir.xy * heightmap_scale * 0.01;
)";
			if (impostor) {
				// Mariner G5b: the true ray (no offset limiting): at the bottom of the relief it has gone xy / z.
				code += "		p = view_dir.xy / max(view_dir.z, 0.05) * heightmap_scale * 0.01;\n";
			}
			code += R"(		vec2 delta = p / num_layers;
		vec2 ofs = base_uv;
)";
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "		float depth = texture(texture_heightmap, ofs).r;\n";
			} else {
				code += "		float depth = 1.0 - texture(texture_heightmap, ofs).r;\n";
			}
			code += R"(
		float current_depth = 0.0;
		while (current_depth < depth) {
			ofs -= delta;
)";
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "			depth = texture(texture_heightmap, ofs).r;\n";
			} else {
				code += "			depth = 1.0 - texture(texture_heightmap, ofs).r;\n";
			}
			code += R"(
			current_depth += layer_depth;
		}

		vec2 prev_ofs = ofs + delta;
		float after_depth = depth - current_depth;
)";
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "		float before_depth = texture(texture_heightmap, prev_ofs).r - current_depth + layer_depth;\n";
			} else {
				code += "		float before_depth = (1.0 - texture(texture_heightmap, prev_ofs).r) - current_depth + layer_depth;\n";
			}
			code += R"(
		float weight = after_depth / (after_depth - before_depth);
		ofs = mix(ofs, prev_ofs, weight);
		float hit_depth = mix(current_depth, current_depth - layer_depth, weight); // Mariner E16.
)";
			if (impostor) {
				// Mariner G5b: the crossing found exactly by a binary search between the last point of the ray above the
				// relief and the first below it (the relief is read texel by texel - no interpolation between layers, whose
				// steps would show on a steep relief seen at grazing angles).
				code += R"(		{
			vec2 above_ofs = prev_ofs;
			float above_depth = current_depth - layer_depth;
			vec2 below_ofs = prev_ofs - delta;
			float below_depth = current_depth;
			for (int i = 0; i < 6; i++) {
				vec2 mid_ofs = (above_ofs + below_ofs) * 0.5;
				float mid_depth = (above_depth + below_depth) * 0.5;
)";
				code += flags[FLAG_INVERT_HEIGHTMAP] ? "				if (mid_depth >= texture(texture_heightmap, mid_ofs).r) {\n" : "				if (mid_depth >= 1.0 - texture(texture_heightmap, mid_ofs).r) {\n";
				code += R"(					below_ofs = mid_ofs;
					below_depth = mid_depth;
				} else {
					above_ofs = mid_ofs;
					above_depth = mid_depth;
				}
			}
			ofs = below_ofs;
			hit_depth = below_depth;
		}
)";
				// Mariner G5b: a ray that has left its face's rectangle went out of the box through another face, and one
				// that reached the bottom of the relief (the impostor's empty texels) saw nothing: the silhouette comes from
				// the relief, not from an alpha scissor (whose filtering would not match the relief's at the edges).
				// Discarded at the end of the fragment function: derivatives after a discard (the relief's depth, the
				// textures' mipmaps) are undefined in their quad.
				code += "		impostor_miss = any(lessThan(ofs, impostor_rect.xy)) || any(greaterThan(ofs, impostor_rect.zw)) || hit_depth > 0.995;\n";
			}

		} else {
			if (flags[FLAG_INVERT_HEIGHTMAP]) {
				code += "		float depth = texture(texture_heightmap, base_uv).r;\n";
			} else {
				code += "		float depth = 1.0 - texture(texture_heightmap, base_uv).r;\n";
			}
			// Use offset limiting to improve the appearance of non-deep parallax.
			// This reduces the impression of depth, but avoids visible warping in the distance.
			// Multiply the heightmap scale by 0.01 to improve heightmap scale usability.
			code += "		vec2 ofs = base_uv - view_dir.xy * depth * heightmap_scale * 0.01;\n";
			code += "		float hit_depth = depth; // Mariner E16.\n";
		}

		// Mariner E16: depth (0 at the top of the relief, 1 at its bottom) of the heightmap at a UV.
		const String depth_at = flags[FLAG_INVERT_HEIGHTMAP] ? "texture(texture_heightmap, %s).r" : "(1.0 - texture(texture_heightmap, %s).r)";
		if (heightmap_self_shadow) {
			// Self-shadowing (after the idea of "steep parallax occlusion mapping" with soft shadows): from the point seen,
			// along the main light up to the top of the relief; relief above the ray blocks the light, nearer more.
			code += R"(
		// Height Self Shadow: Enabled
		{
			vec3 light_t = normalize(MAIN_LIGHT_DIRECTION * mat3(TANGENT * heightmap_flip.x, -BINORMAL * heightmap_flip.y, NORMAL));
			if (light_t.z > 0.0) {
				const int shadow_steps = 12;
				float d_step = hit_depth / float(shadow_steps);
				vec2 uv_step = light_t.xy / max(light_t.z, 0.1) * heightmap_scale * 0.01 * d_step;
				vec2 shadow_uv = ofs;
				float ray_depth = hit_depth;
				float occlusion = 0.0;
				for (int i = 1; i <= shadow_steps; i++) {
					shadow_uv += uv_step;
					ray_depth -= d_step;
)";
			code += "					float surface_depth = " + vformat(depth_at, "shadow_uv") + ";\n";
			code += R"(
					occlusion = max(occlusion, (ray_depth - surface_depth) * (1.0 - float(i) / float(shadow_steps + 1)));
				}
				MAIN_LIGHT_SHADOW = 1.0 - clamp(occlusion * 20.0, 0.0, 1.0) * heightmap_self_shadow_strength;
			}
		}
)";
		}
		if (heightmap_depth_offset) {
			// The seen point of the relief, below the mesh surface, as far along the view ray as the parallax shifted it:
			// its depth (intersections, contact shadows, SSAO) and the position its light is computed at (shadow maps).
			code += R"(
		// Height Depth Offset: Enabled
		{
			float world_per_uv = sqrt(length(dFdx(VERTEX)) * length(dFdy(VERTEX)) / max(length(dFdx(height_base_uv)) * length(dFdy(height_base_uv)), 1e-12));
			vec3 relief_vertex = VERTEX - normalize(-VERTEX + EYE_OFFSET) * (hit_depth * abs(heightmap_scale) * 0.01 * world_per_uv);
)";
			if (impostor) {
				// Mariner G5b: along the true ray, as deep below the face as the hit.
				code += "			relief_vertex = VERTEX - impostor_eye * (hit_depth * abs(heightmap_scale) * 0.01 * world_per_uv / max(view_dir.z, 0.05));\n";
			}
			code += R"(
			LIGHT_VERTEX = relief_vertex;
			vec4 relief_clip = PROJECTION_MATRIX * vec4(relief_vertex, 1.0);
			DEPTH = relief_clip.z / relief_clip.w;
)";
			if (OS::get_singleton()->get_current_rendering_method() == "gl_compatibility") {
				code += "			DEPTH = DEPTH * 0.5 + 0.5; // Mariner G5e: OpenGL's clip space depth is -1 .. 1.\n";
			}
			code += R"(		}
)";
		}

		code += "		base_uv = ofs;\n";
		if (features[FEATURE_DETAIL] && detail_uv == DETAIL_UV_2) {
			code += "		base_uv2 -= ofs;\n";
		}

		code += "	}\n";
	}

	if (impostor_uv) {
		String height_code = code.substr(height_code_start);
		for (const char *uv : { "ofs", "prev_ofs", "mid_ofs", "shadow_uv", "base_uv" }) {
			height_code = height_code.replace(vformat("texture(texture_heightmap, %s)", uv), vformat("textureGrad(texture_heightmap, %s, impostor_dx, impostor_dy)", uv));
		}
		code = code.substr(0, height_code_start) + height_code;
	}

	if (flags[FLAG_USE_POINT_SIZE]) {
		code += R"(
	// Use Point Size: Enabled
	vec4 albedo_tex = texture(texture_albedo, POINT_COORD);
)";
	} else {
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += R"(
	vec4 albedo_tex = triplanar_texture(texture_albedo, uv1_power_normal, uv1_triplanar_pos);
)";
		} else {
			code += impostor_uv ? "\n	vec4 albedo_tex = textureGrad(texture_albedo, base_uv, impostor_dx, impostor_dy);\n" : R"(
	vec4 albedo_tex = texture(texture_albedo, base_uv);
)";
		}
	}

	if (flags[FLAG_ALBEDO_TEXTURE_MSDF] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		code += R"(
	{
		// Albedo Texture MSDF: Enabled
		if (!OUTPUT_IS_SRGB) {
			albedo_tex.rgb = mix(
					vec3(1.0 + 0.055) * pow(albedo_tex.rgb, vec3(1.0 / 2.4)) - vec3(0.055),
					vec3(12.92) * albedo_tex.rgb,
					lessThan(albedo_tex.rgb, vec3(0.0031308)));
		}
		vec2 msdf_size = vec2(msdf_pixel_range) / vec2(albedo_texture_size);
)";
		if (flags[FLAG_USE_POINT_SIZE]) {
			code += "		vec2 dest_size = vec2(1.0) / fwidth(POINT_COORD);\n";
		} else {
			code += "		vec2 dest_size = vec2(1.0) / fwidth(base_uv);\n";
		}
		code += R"(
		float px_size = max(0.5 * dot(msdf_size, dest_size), 1.0);
		float d = msdf_median(albedo_tex.r, albedo_tex.g, albedo_tex.b);
		if (msdf_outline_size > 0.0) {
			float cr = clamp(msdf_outline_size, 0.0, (msdf_pixel_range / 2.0) - 1.0) / msdf_pixel_range;
			d = min(d, albedo_tex.a);
			albedo_tex.a = clamp((d - 0.5 + cr) * px_size, 0.0, 1.0);
		} else {
			albedo_tex.a = clamp((d - 0.5) * px_size + 0.5, 0.0, 1.0);
		}
		albedo_tex.rgb = vec3(1.0);
	}
)";
	} else if (flags[FLAG_ALBEDO_TEXTURE_FORCE_SRGB]) {
		code += R"(
	// Albedo Texture Force sRGB: Enabled
	albedo_tex.rgb = mix(
			pow((albedo_tex.rgb + vec3(0.055)) * (1.0 / (1.0 + 0.055)), vec3(2.4)),
			albedo_tex.rgb.rgb * (1.0 / 12.92),
			lessThan(albedo_tex.rgb, vec3(0.04045)));
)";
	}

	if (flags[FLAG_ALBEDO_FROM_VERTEX_COLOR]) {
		code += R"(
	// Vertex Color Use as Albedo: Enabled
	albedo_tex *= COLOR;

)";
	}
	code += "	ALBEDO = albedo.rgb * albedo_tex.rgb;\n";

	if (!orm) {
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += R"(
	float metallic_tex = dot(triplanar_texture(texture_metallic, uv1_power_normal, uv1_triplanar_pos), metallic_texture_channel);
)";
		} else {
			code += R"(
	float metallic_tex = dot(texture(texture_metallic, base_uv), metallic_texture_channel);
)";
		}

		code += R"(	METALLIC = metallic_tex * metallic;
	SPECULAR = specular;
)";
		switch (roughness_texture_channel) {
			case TEXTURE_CHANNEL_RED: {
				code += R"(
	vec4 roughness_texture_channel = vec4(1.0, 0.0, 0.0, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_GREEN: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.0, 1.0, 0.0, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_BLUE: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.0, 0.0, 1.0, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_ALPHA: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.0, 0.0, 0.0, 1.0);
)";
			} break;
			case TEXTURE_CHANNEL_GRAYSCALE: {
				code += R"(
	vec4 roughness_texture_channel = vec4(0.333333, 0.333333, 0.333333, 0.0);
)";
			} break;
			case TEXTURE_CHANNEL_MAX:
				break; // Internal value, skip.
		}

		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	float roughness_tex = dot(triplanar_texture(texture_roughness, uv1_power_normal, uv1_triplanar_pos), roughness_texture_channel);\n";
		} else {
			code += "	float roughness_tex = dot(texture(texture_roughness, base_uv), roughness_texture_channel);\n";
		}
		code += R"(	ROUGHNESS = roughness_tex * roughness;
)";
	} else {
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += R"(
	vec4 orm_tex = triplanar_texture(texture_orm, uv1_power_normal, uv1_triplanar_pos);
)";
		} else {
			code += R"(
	vec4 orm_tex = texture(texture_orm, base_uv);
)";
		}

		code += R"(	ROUGHNESS = orm_tex.g;
 	METALLIC = orm_tex.b;
)";
	}

	if (features[FEATURE_NORMAL_MAPPING] && !impostor_hull) {
		code += R"(
	// Normal Map: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	NORMAL_MAP = triplanar_texture(texture_normal, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
		} else {
			code += impostor_uv ? "	NORMAL_MAP = textureGrad(texture_normal, base_uv, impostor_dx, impostor_dy).rgb;\n" : "	NORMAL_MAP = texture(texture_normal, base_uv).rgb;\n";
		}
		code += "	NORMAL_MAP_DEPTH = normal_scale;\n";
	}
	if (impostor_hull) {
		// Mariner G5c: the hit's normal (the seeing face's normal map, in object space), not the drawn face's.
		code += "	NORMAL = normalize((VIEW_MATRIX * (MODEL_MATRIX * vec4(impostor_normal_obj, 0.0))).xyz);\n";
	}

	if (features[FEATURE_BENT_NORMAL_MAPPING]) {
		code += R"(
	// Bent Normal Map: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	BENT_NORMAL_MAP = triplanar_texture(texture_bent_normal, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
		} else {
			code += "	BENT_NORMAL_MAP = texture(texture_bent_normal, base_uv).rgb;\n";
		}
	}

	if (features[FEATURE_EMISSION]) {
		code += R"(
	// Emission: Enabled
)";
		if (flags[FLAG_EMISSION_ON_UV2]) {
			if (flags[FLAG_UV2_USE_TRIPLANAR]) {
				code += "	vec3 emission_tex = triplanar_texture(texture_emission, uv2_power_normal, uv2_triplanar_pos).rgb;\n";
			} else {
				code += "	vec3 emission_tex = texture(texture_emission, base_uv2).rgb;\n";
			}
		} else {
			if (flags[FLAG_UV1_USE_TRIPLANAR]) {
				code += "	vec3 emission_tex = triplanar_texture(texture_emission, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
			} else {
				code += "	vec3 emission_tex = texture(texture_emission, base_uv).rgb;\n";
			}
		}

		if (emission_op == EMISSION_OP_ADD) {
			code += R"(	// Emission Operator: Add
	EMISSION = (emission.rgb + emission_tex) * emission_energy;
)";
		} else {
			code += R"(	// Emission Operator: Multiply
	EMISSION = (emission.rgb * emission_tex) * emission_energy;
)";
		}
	}

	if (features[FEATURE_REFRACTION]) {
		if (features[FEATURE_NORMAL_MAPPING]) {
			code += R"(
	// Refraction: Enabled (with normal map texture)
	vec3 unpacked_normal = NORMAL_MAP;
	unpacked_normal.xy = unpacked_normal.xy * 2.0 - 1.0;
	unpacked_normal.z = sqrt(max(0.0, 1.0 - dot(unpacked_normal.xy, unpacked_normal.xy)));
	vec3 ref_normal = normalize(mix(
			NORMAL,
			TANGENT * unpacked_normal.x + BINORMAL * unpacked_normal.y + NORMAL * unpacked_normal.z,
			NORMAL_MAP_DEPTH));
)";
		} else {
			code += R"(
	// Refraction: Enabled
	vec3 ref_normal = NORMAL;
)";
		}
		if (refraction_physical) {
			// Mariner F6: refraction by the index of refraction through a slab of the given thickness (0: a thin pane),
			// absorption by distance inside (Beer-Lambert, as KHR_materials_volume), Fresnel of the interface, tint by the
			// albedo, blur by the roughness. Ideas after Filament's screen-space refraction; own code.
			code += R"(
	float ref_amount = 1.0 - albedo.a * albedo_tex.a;
	vec3 ref_view = normalize(-VERTEX);
	vec3 ref_dir = refract(-ref_view, ref_normal, 1.0 / refraction_ior);
	float ref_cos = max(dot(-ref_normal, ref_dir), 0.05);
	float ref_path = refraction_thickness / ref_cos;
	vec3 ref_exit = VERTEX + ref_dir * ref_path;
	vec4 ref_clip = PROJECTION_MATRIX * vec4(ref_exit, 1.0);
	vec2 ref_ofs = clamp(ref_clip.xy / ref_clip.w * 0.5 + 0.5, vec2(0.0), vec2(1.0));

	float refraction_depth_tex = textureLod(depth_texture, ref_ofs, 0.0).r;
	vec4 ndc = OUTPUT_IS_SRGB ? vec4(vec3(ref_ofs, refraction_depth_tex) * 2.0 - 1.0, 1.0) : vec4(ref_ofs * 2.0 - 1.0, refraction_depth_tex, 1.0);
	vec4 refraction_view_pos = INV_PROJECTION_MATRIX * ndc;
	refraction_view_pos.xyz /= refraction_view_pos.w;
	// Something in front of the surface at the refracted point: use the point behind the pixel instead.
	ref_ofs = mix(SCREEN_UV, ref_ofs, smoothstep(0.0, 0.05, VERTEX.z - refraction_view_pos.z));

	vec3 ref_sigma = -log(max(refraction_absorption_color.rgb, vec3(1e-4))) / max(refraction_absorption_distance, 1e-4);
	vec3 ref_transmittance = exp(-ref_sigma * ref_path);
	float ref_f0 = (refraction_ior - 1.0) / (refraction_ior + 1.0);
	ref_f0 *= ref_f0;
	float ref_fresnel = ref_f0 + (1.0 - ref_f0) * pow(1.0 - clamp(dot(ref_normal, ref_view), 0.0, 1.0), 5.0);
	EMISSION += textureLod(screen_texture, ref_ofs, ROUGHNESS * 8.0).rgb * ref_transmittance * ALBEDO * (1.0 - ref_fresnel) * ref_amount * EXPOSURE;
	ALBEDO *= 1.0 - ref_amount;
	// The specular reflection of the interface follows the index of refraction (F0 = 0.16 * specular^2).
	SPECULAR = mix(SPECULAR, clamp(sqrt(ref_f0 / 0.16), 0.0, 1.0), ref_amount);
	// Force transparency on the material (required for refraction).
	ALPHA = 1.0;
)";
		} else {
			if (flags[FLAG_UV1_USE_TRIPLANAR]) {
				code += "	vec2 ref_ofs = SCREEN_UV - ref_normal.xy * dot(triplanar_texture(texture_refraction, uv1_power_normal, uv1_triplanar_pos), refraction_texture_channel) * refraction;\n";
			} else {
				code += "	vec2 ref_ofs = SCREEN_UV - ref_normal.xy * dot(texture(texture_refraction, base_uv), refraction_texture_channel) * refraction;\n";
			}
			code += R"(
	float ref_amount = 1.0 - albedo.a * albedo_tex.a;

	float refraction_depth_tex = textureLod(depth_texture, ref_ofs, 0.0).r;
	vec4 ndc = OUTPUT_IS_SRGB ? vec4(vec3(SCREEN_UV, refraction_depth_tex) * 2.0 - 1.0, 1.0) : vec4(SCREEN_UV * 2.0 - 1.0, refraction_depth_tex, 1.0);
	vec4 refraction_view_pos = INV_PROJECTION_MATRIX * ndc;
	refraction_view_pos.xyz /= refraction_view_pos.w;

	// If the depth buffer is lower then the model's Z position, use the refracted UV, otherwise use the normal screen UV.
	// At low depth differences, decrease refraction intensity to avoid sudden discontinuities.
	EMISSION += textureLod(screen_texture, mix(SCREEN_UV, ref_ofs, smoothstep(0.0, 1.0, VERTEX.z - refraction_view_pos.z)), ROUGHNESS * 8.0).rgb * ref_amount * EXPOSURE;
	ALBEDO *= 1.0 - ref_amount;
	// Force transparency on the material (required for refraction).
	ALPHA = 1.0;
)";
		}

	} else if (transparency != TRANSPARENCY_DISABLED || flags[FLAG_USE_SHADOW_TO_OPACITY] || (distance_fade == DISTANCE_FADE_PIXEL_ALPHA) || proximity_fade_enabled) {
		if (alpha_mask_mode != ALPHA_MASK_ALBEDO && transparency != TRANSPARENCY_DISABLED) {
			// Mariner AA2: a distance field instead of the albedo alpha (0.5 on the contour).
			if (alpha_mask_mode == ALPHA_MASK_MSDF) {
				code += "	vec3 alpha_mask_tex = texture(texture_alpha_mask, base_uv).rgb;\n";
				code += "	ALPHA *= albedo.a * max(min(alpha_mask_tex.r, alpha_mask_tex.g), min(max(alpha_mask_tex.r, alpha_mask_tex.g), alpha_mask_tex.b));\n";
			} else {
				code += "	ALPHA *= albedo.a * texture(texture_alpha_mask, base_uv).r;\n";
			}
		} else {
			code += "	ALPHA *= albedo.a * albedo_tex.a;\n";
		}
	}
	if (transparency == TRANSPARENCY_ALPHA_HASH) {
		code += "	ALPHA_HASH_SCALE = alpha_hash_scale;\n";
	} else if (transparency == TRANSPARENCY_ALPHA_SCISSOR) {
		code += "	ALPHA_SCISSOR_THRESHOLD = alpha_scissor_threshold;\n";
	}
	if (alpha_antialiasing_mode == ALPHA_ANTIALIASING_FRINGE) {
		if (transparency == TRANSPARENCY_ALPHA_SCISSOR) {
			if (alpha_mask_mode == ALPHA_MASK_ALBEDO) {
				// Mariner AA: the albedo texture's texels drive the mip selector (far away the edge is the mask's own coverage).
				code += "	ALPHA_FRINGE_COORDINATE = UV * vec2(albedo_texture_size);\n";
			} else {
				// Mariner AA2: a distance field is not coverage - no mip selector (the sharpened edge at every distance).
				code += "	ALPHA_FRINGE_FAR = vec2(0.0);\n";
			}
		}
	} else if (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF && (transparency == TRANSPARENCY_ALPHA_HASH || transparency == TRANSPARENCY_ALPHA_SCISSOR)) {
		code += "	ALPHA_ANTIALIASING_EDGE = alpha_antialiasing_edge;\n";
		code += "	ALPHA_TEXTURE_COORDINATE = UV * vec2(albedo_texture_size);\n";
	}

	if (wire_aa) {
		code += "	ALPHA *= clamp(wire_r_px - abs(wire_u_px) + 0.5, 0.0, 1.0) * wire_fade; // Mariner AA3: wire antialiasing.\n";
	}

	if (proximity_fade_enabled) {
		code += R"(
	// Proximity Fade: Enabled
	float proximity_depth_tex = textureLod(depth_texture, SCREEN_UV, 0.0).r;
	vec4 ndc = OUTPUT_IS_SRGB ? vec4(vec3(SCREEN_UV, proximity_depth_tex) * 2.0 - 1.0, 1.0) : vec4(SCREEN_UV * 2.0 - 1.0, proximity_depth_tex, 1.0);
	vec4 proximity_view_pos = INV_PROJECTION_MATRIX * ndc;
	proximity_view_pos.xyz /= proximity_view_pos.w;
	ALPHA *= clamp(1.0 - smoothstep(proximity_view_pos.z + proximity_fade_distance, proximity_view_pos.z, VERTEX.z), 0.0, 1.0);
)";
	}

	if (distance_fade != DISTANCE_FADE_DISABLED) {
		// Use the slightly more expensive circular fade (distance to the object) instead of linear
		// (Z distance), so that the fade is always the same regardless of the camera angle.
		if ((distance_fade == DISTANCE_FADE_OBJECT_DITHER || distance_fade == DISTANCE_FADE_PIXEL_DITHER)) {
			code += "\n	{";

			if (distance_fade == DISTANCE_FADE_OBJECT_DITHER) {
				code += R"(
		// Distance Fade: Object Dither
		float fade_distance = length((VIEW_MATRIX * MODEL_MATRIX[3]));
)";
			} else {
				code += R"(
		// Distance Fade: Pixel Dither
		float fade_distance = length(VERTEX);
)";
			}
			code += R"(
		// Use interleaved gradient noise, which is fast but still looks good.
		const vec3 magic = vec3(0.06711056, 0.00583715, 52.9829189);
		float fade = clamp(smoothstep(distance_fade_min, distance_fade_max, fade_distance), 0.0, 1.0);
		// Use a hard cap to prevent a few stray pixels from remaining when past the fade-out distance.
		if (fade < 0.001 || fade < fract(magic.z * fract(dot(FRAGCOORD.xy, magic.xy)))) {
			discard;
		}
	}
)";
		} else {
			code += R"(
	// Distance Fade: Pixel Alpha
	ALPHA *= clamp(smoothstep(distance_fade_min, distance_fade_max, length(VERTEX)), 0.0, 1.0);
)";
		}
	}

	if (features[FEATURE_RIM]) {
		code += R"(
	// Rim: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec2 rim_tex = triplanar_texture(texture_rim, uv1_power_normal, uv1_triplanar_pos).xy;\n";
		} else {
			code += "	vec2 rim_tex = texture(texture_rim, base_uv).xy;\n";
		}
		code += R"(	RIM = rim * rim_tex.x;
	RIM_TINT = rim_tint * rim_tex.y;
)";
	}

	if (features[FEATURE_CLEARCOAT]) {
		code += R"(
	// Clearcoat: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec2 clearcoat_tex = triplanar_texture(texture_clearcoat, uv1_power_normal, uv1_triplanar_pos).xy;\n";
		} else {
			code += "	vec2 clearcoat_tex = texture(texture_clearcoat, base_uv).xy;\n";
		}
		code += R"(	CLEARCOAT = clearcoat * clearcoat_tex.x;
	CLEARCOAT_ROUGHNESS = clearcoat_roughness * clearcoat_tex.y;
)";
	}

	if (features[FEATURE_SHEEN]) {
		code += "\n	// Sheen: Enabled (Mariner M6)\n";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec4 sheen_tex = triplanar_texture(texture_sheen, uv1_power_normal, uv1_triplanar_pos);\n";
		} else {
			code += "	vec4 sheen_tex = texture(texture_sheen, base_uv);\n";
		}
		code += R"(	SHEEN = sheen_color.rgb * sheen_tex.rgb;
	SHEEN_ROUGHNESS = sheen_roughness * sheen_tex.a;
)";
	}

	if (features[FEATURE_IRIDESCENCE]) {
		code += "\n	// Iridescence: Enabled (Mariner F5)\n";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec2 iridescence_tex = triplanar_texture(texture_iridescence, uv1_power_normal, uv1_triplanar_pos).rg;\n";
		} else {
			code += "	vec2 iridescence_tex = texture(texture_iridescence, base_uv).rg;\n";
		}
		code += R"(	IRIDESCENCE = iridescence * iridescence_tex.r;
	IRIDESCENCE_IOR = iridescence_ior;
	IRIDESCENCE_THICKNESS = mix(iridescence_thickness_min, iridescence_thickness_max, iridescence_tex.g);
)";
	}

	if (features[FEATURE_ANISOTROPY]) {
		code += R"(
	// Anisotropy: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec3 anisotropy_tex = triplanar_texture(texture_flowmap, uv1_power_normal, uv1_triplanar_pos).rga;\n";
		} else {
			code += "	vec3 anisotropy_tex = texture(texture_flowmap, base_uv).rga;\n";
		}
		code += R"(	ANISOTROPY = anisotropy_ratio * anisotropy_tex.b;
	ANISOTROPY_FLOW = anisotropy_tex.rg * 2.0 - 1.0;
)";
	}

	if (features[FEATURE_AMBIENT_OCCLUSION]) {
		code += R"(
	// Ambient Occlusion: Enabled
)";
		if (!orm) {
			if (flags[FLAG_AO_ON_UV2]) {
				if (flags[FLAG_UV2_USE_TRIPLANAR]) {
					code += "	AO = dot(triplanar_texture(texture_ambient_occlusion, uv2_power_normal, uv2_triplanar_pos), ao_texture_channel);\n";
				} else {
					code += "	AO = dot(texture(texture_ambient_occlusion, base_uv2), ao_texture_channel);\n";
				}
			} else {
				if (flags[FLAG_UV1_USE_TRIPLANAR]) {
					code += "	AO = dot(triplanar_texture(texture_ambient_occlusion, uv1_power_normal, uv1_triplanar_pos), ao_texture_channel);\n";
				} else {
					code += "	AO = dot(texture(texture_ambient_occlusion, base_uv), ao_texture_channel);\n";
				}
			}
		} else {
			code += "	AO = orm_tex.r;\n";
		}

		code += "	AO_LIGHT_AFFECT = ao_light_affect;\n";
	}

	if (features[FEATURE_SUBSURFACE_SCATTERING]) {
		code += R"(
	// Subsurface Scattering: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	float sss_tex = triplanar_texture(texture_subsurface_scattering, uv1_power_normal, uv1_triplanar_pos).r;\n";
		} else {
			code += "	float sss_tex = texture(texture_subsurface_scattering, base_uv).r;\n";
		}
		code += "	SSS_STRENGTH = subsurface_scattering_strength * sss_tex;\n";
	}

	if (features[FEATURE_SUBSURFACE_TRANSMITTANCE]) {
		code += R"(
	// Subsurface Scattering Transmittance: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec4 trans_color_tex = triplanar_texture(texture_subsurface_transmittance, uv1_power_normal, uv1_triplanar_pos);\n";
		} else {
			code += "	vec4 trans_color_tex = texture(texture_subsurface_transmittance, base_uv);\n";
		}
		code += "	SSS_TRANSMITTANCE_COLOR = transmittance_color * trans_color_tex;\n";

		code += R"(	SSS_TRANSMITTANCE_DEPTH = transmittance_depth;
	SSS_TRANSMITTANCE_BOOST = transmittance_boost;
)";
	}

	if (features[FEATURE_BACKLIGHT]) {
		code += R"(
	// Backlight: Enabled
)";
		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec3 backlight_tex = triplanar_texture(texture_backlight, uv1_power_normal, uv1_triplanar_pos).rgb;\n";
		} else {
			code += "	vec3 backlight_tex = texture(texture_backlight, base_uv).rgb;\n";
		}
		code += "	BACKLIGHT = (backlight.rgb + backlight_tex);\n";
	}

	if (features[FEATURE_DETAIL]) {
		code += R"(
	// Detail: Enabled
)";
		const bool triplanar = (flags[FLAG_UV1_USE_TRIPLANAR] && detail_uv == DETAIL_UV_1) || (flags[FLAG_UV2_USE_TRIPLANAR] && detail_uv == DETAIL_UV_2);
		if (triplanar) {
			const String tp_uv = detail_uv == DETAIL_UV_1 ? "uv1" : "uv2";
			code += vformat(R"(	vec4 detail_tex = triplanar_texture(texture_detail_albedo, %s_power_normal, %s_triplanar_pos);
	vec4 detail_norm_tex = triplanar_texture(texture_detail_normal, %s_power_normal, %s_triplanar_pos);
)",
					tp_uv, tp_uv, tp_uv, tp_uv);
		} else {
			const String det_uv = detail_uv == DETAIL_UV_1 ? "base_uv" : "base_uv2";
			code += vformat(R"(	vec4 detail_tex = texture(texture_detail_albedo, %s);
	vec4 detail_norm_tex = texture(texture_detail_normal, %s);
)",
					det_uv, det_uv);
		}

		if (flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += "	vec4 detail_mask_tex = triplanar_texture(texture_detail_mask, uv1_power_normal, uv1_triplanar_pos);\n";
		} else {
			code += "	vec4 detail_mask_tex = texture(texture_detail_mask, base_uv);\n";
		}

		switch (detail_blend_mode) {
			case BLEND_MODE_MIX: {
				code += R"(
	// Detail Blend Mode: Mix
	vec3 detail = mix(ALBEDO.rgb, detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_ADD: {
				code += R"(
	// Detail Blend Mode: Add
	vec3 detail = mix(ALBEDO.rgb, ALBEDO.rgb + detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_SUB: {
				code += R"(
	// Detail Blend Mode: Subtract
	vec3 detail = mix(ALBEDO.rgb, ALBEDO.rgb - detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_MUL: {
				code += R"(
	// Detail Blend Mode: Multiply
	vec3 detail = mix(ALBEDO.rgb, ALBEDO.rgb * detail_tex.rgb, detail_tex.a);
)";
			} break;
			case BLEND_MODE_PREMULT_ALPHA: {
				// This is unlikely to ever be used for detail textures, and in order for it to function in the editor, another bit must be used in MaterialKey,
				// but there are only 5 bits left, so I'm going to leave this disabled unless it's actually requested.
				//code += "\tvec3 detail = (1.0-detail_tex.a)*ALBEDO.rgb+detail_tex.rgb;\n";
			} break;
			case BLEND_MODE_MAX:
				break; // Internal value, skip.
		}

		if (detail_normal_rnm) {
			// Mariner M4: reoriented normal mapping (Barre-Brisebois and Hill 2012): the detail normal is rotated onto the
			// base normal instead of being averaged with it, so the base relief keeps its depth under the detail.
			code += R"(	vec3 rnm_t = vec3(NORMAL_MAP.xy * 2.0 - 1.0, 0.0);
	rnm_t.z = sqrt(max(0.0, 1.0 - dot(rnm_t.xy, rnm_t.xy))) + 1.0;
	vec3 rnm_u = vec3(detail_norm_tex.xy * 2.0 - 1.0, 0.0);
	rnm_u.z = sqrt(max(0.0, 1.0 - dot(rnm_u.xy, rnm_u.xy)));
	rnm_u.xy = -rnm_u.xy;
	vec3 rnm = normalize(rnm_t * dot(rnm_t, rnm_u) / rnm_t.z - rnm_u);
	vec3 detail_norm = mix(NORMAL_MAP, rnm * 0.5 + 0.5, detail_tex.a);
)";
		} else {
			code += "	vec3 detail_norm = mix(NORMAL_MAP, detail_norm_tex.rgb, detail_tex.a);\n";
		}
		code += R"(	NORMAL_MAP = mix(NORMAL_MAP, detail_norm, detail_mask_tex.r);
	ALBEDO.rgb = mix(ALBEDO.rgb, detail, detail_mask_tex.r);
)";
	}

	if (features[FEATURE_INTERIOR] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
		// Mariner G5a: fake interiors (after the published technique of interior mapping, van Dongen 2008, with one
		// image per room as in later games; own implementation). The view ray enters a box room behind every window
		// cell of the UV grid and is intersected with its walls, floor, ceiling and back wall; the hit point is looked up
		// in an image of the room as seen from the window (a perspective view whose back wall takes interior_back_wall
		// of the frame). A random room of the atlas, mirrored for every other window, lit or not, per window.
		code += R"(
	// Interior: Enabled (Mariner G5a)
	{
)";
		code += impostor_uv ? "		float interior_glass = textureGrad(texture_interior_mask, base_uv, impostor_dx, impostor_dy).r;\n" : "		float interior_glass = texture(texture_interior_mask, base_uv).r;\n";
		code += R"(
		// The view direction in the UV frame (x along U, y along V, z out of the surface), as for the height map.
		vec3 interior_view = normalize(normalize(-VERTEX + EYE_OFFSET) * mat3(TANGENT, -BINORMAL, NORMAL));
		vec2 interior_cell_uv = base_uv * interior_rooms;
		vec2 interior_cell = floor(interior_cell_uv);
		vec3 interior_p = vec3(interior_cell_uv - interior_cell, 0.0);
		// The ray into the room, in room units (the box is 0 .. 1 on every axis; z is the depth).
		vec3 interior_dir = vec3(-interior_view.xy, max(interior_view.z, 0.001)) / max(interior_room_size, vec3(0.01));
		vec3 interior_t3 = (step(0.0, interior_dir) - interior_p) / interior_dir;
		float interior_t = min(min(interior_t3.x, interior_t3.y), interior_t3.z);
		vec3 interior_hit = clamp(interior_p + interior_dir * interior_t, 0.0, 1.0);
		// A random number per window (and per instance, so that buildings sharing the material differ).
		ivec2 interior_node = ivec2(floor(NODE_POSITION_WORLD.xz));
		uint interior_h = uint(int(interior_cell.x) + 4096 * int(interior_cell.y)) ^ (uint(interior_node.x) * 2654435761u) ^ (uint(interior_node.y) * 2246822519u);
		interior_h = interior_h * 747796405u + 2891336453u;
		interior_h = ((interior_h >> ((interior_h >> 28u) + 4u)) ^ interior_h) * 277803737u;
		interior_h = (interior_h >> 22u) ^ interior_h;
		float interior_r1 = float(interior_h & 65535u) / 65536.0;
		float interior_r2 = float((interior_h >> 16u) & 32767u) / 32768.0;
		// The hit point as seen from the window in the room's image.
		float interior_scale = 1.0 / (1.0 + interior_hit.z * (1.0 / interior_back_wall - 1.0));
		vec2 interior_c = interior_hit.xy - 0.5;
		interior_c.x = (interior_h & 2147483648u) != 0u ? -interior_c.x : interior_c.x;
		vec2 interior_room_uv = 0.5 + interior_c * interior_scale;
		// The room's tile in the atlas, kept half a texel away from its neighbours.
		ivec2 interior_atlas = max(interior_atlas_size, ivec2(1));
		int interior_index = min(int(interior_r1 * float(interior_atlas.x * interior_atlas.y)), interior_atlas.x * interior_atlas.y - 1);
		vec2 interior_tile = vec2(float(interior_index % interior_atlas.x), float(interior_index / interior_atlas.x));
		vec2 interior_tile_size = 1.0 / vec2(interior_atlas);
		vec2 interior_margin = 0.5 / (vec2(textureSize(texture_interior, 0)) * interior_tile_size);
		interior_room_uv = clamp(interior_room_uv, interior_margin, 1.0 - interior_margin);
		// Gradients of the window grid (not of the room lookup, which jumps between walls and windows).
		vec2 interior_grad_x = dFdx(interior_cell_uv) * interior_tile_size * interior_scale;
		vec2 interior_grad_y = dFdy(interior_cell_uv) * interior_tile_size * interior_scale;
		vec3 interior_room = textureGrad(texture_interior, (interior_tile + interior_room_uv) * interior_tile_size, interior_grad_x, interior_grad_y).rgb;
		float interior_lit = interior_r2 < interior_lit_fraction ? 1.0 : 0.0;
		vec3 interior_light = interior_room * (interior_brightness + interior_lit * interior_lights * interior_light_color.rgb);
		// Mariner G5d: curtains or blinds in some windows, 15 cm behind the glass: drawn to a random width (curtains,
		// folded, from both sides) or lowered to a random height, the slats more or less closed (blinds). Lit from the room
		// behind them when its lamps are on.
		uint interior_h2 = interior_h * 747796405u + 2891336453u;
		interior_h2 = ((interior_h2 >> ((interior_h2 >> 28u) + 4u)) ^ interior_h2) * 277803737u;
		interior_h2 = (interior_h2 >> 22u) ^ interior_h2;
		float interior_r3 = float(interior_h2 & 65535u) / 65536.0;
		float interior_r4 = float((interior_h2 >> 16u) & 32767u) / 32768.0;
		if (interior_r3 < interior_curtains + interior_blinds) {
			vec2 interior_cp = interior_p.xy + interior_dir.xy * ((0.15 / max(interior_room_size.z, 0.01)) / interior_dir.z);
			float interior_cover;
			float interior_shade;
			if (interior_r3 < interior_curtains) {
				float interior_half = 0.5 * (0.25 + 0.75 * interior_r4); // Each side covers this much of the width (drawn open: 12.5 %).
				float interior_side = min(interior_cp.x, 1.0 - interior_cp.x);
				interior_cover = 1.0 - smoothstep(interior_half - 0.01, interior_half + 0.01, interior_side);
				interior_shade = 0.82 + 0.18 * sin(interior_cp.x * 70.0 + interior_r4 * 20.0);
			} else {
				float interior_lowered = 0.25 + 0.75 * interior_r4; // From the top of the window (V grows down).
				float interior_slat = fract(interior_cp.y * 14.0);
				interior_cover = (1.0 - smoothstep(interior_lowered - 0.01, interior_lowered + 0.01, interior_cp.y)) * step(interior_slat, mix(0.55, 0.95, (interior_r3 - interior_curtains) / max(interior_blinds, 0.001)));
				interior_shade = 0.75 + 0.25 * interior_slat;
			}
			interior_cover *= step(0.0, interior_cp.x) * step(interior_cp.x, 1.0) * step(0.0, interior_cp.y) * step(interior_cp.y, 1.0);
			vec3 interior_curtain = interior_curtain_color.rgb * interior_shade * (interior_brightness * 0.6 + interior_lit * interior_lights * 0.5 * interior_light_color.rgb);
			interior_light = mix(interior_light, interior_curtain, interior_cover);
		}
)";
		if (shading_mode == SHADING_MODE_UNSHADED) {
			code += "		ALBEDO = mix(ALBEDO, interior_light, interior_glass);\n";
		} else {
			code += R"(		// The glass: dark and smooth, it reflects the world; the room shows through it, less at grazing angles.
		float interior_fresnel = 0.04 + 0.96 * pow(1.0 - clamp(interior_view.z, 0.0, 1.0), 5.0);
		ALBEDO = mix(ALBEDO, vec3(0.0), interior_glass);
		ROUGHNESS = mix(ROUGHNESS, 0.05, interior_glass);
		METALLIC = mix(METALLIC, 0.0, interior_glass);
		EMISSION += interior_light * (interior_glass * (1.0 - interior_fresnel));
)";
		}
		code += "	}\n";
	}

	if (features[FEATURE_WETNESS] && shading_mode != SHADING_MODE_UNSHADED) {
		// Mariner G4: wet surfaces (after the published model of porosity and puddles, e.g. Lagarde's "Water drop"
		// articles; own implementation).
		code += R"(
	// Wetness: Enabled (Mariner G4)
	{
		// The world's wetness, not under cover (no rain there).
		float wet = clamp(WETNESS * wetness_amount * (1.0 - OVERHEAD_COVER), 0.0, 1.0);
		float facing_up = clamp((INV_VIEW_MATRIX * vec4(NORMAL, 0.0)).y, 0.0, 1.0);
		// Porous surfaces soak water up and darken; a film of water on top makes them smoother, less on steep faces (it
		// runs off) and on porous ones (they drink it).
		ALBEDO *= mix(1.0, 0.45, wet * wetness_porosity);
		float wet_film = wet * mix(0.5, 1.0, facing_up) * (1.0 - 0.5 * wetness_porosity);
		ROUGHNESS = mix(ROUGHNESS, 0.06, wet_film);
)";
		if (features[FEATURE_HEIGHT_MAPPING] && !flags[FLAG_UV1_USE_TRIPLANAR]) {
			code += flags[FLAG_INVERT_HEIGHTMAP] ? "		float wet_height = 1.0 - texture(texture_heightmap, base_uv).r;\n" : "		float wet_height = texture(texture_heightmap, base_uv).r;\n";
			code += R"(		// Puddles on flat ground where the height map is below the water level (rising with the wetness).
		float wet_level = wetness_puddles * wet;
		float puddle = (1.0 - smoothstep(wet_level - 0.05, wet_level, wet_height)) * smoothstep(0.85, 0.97, facing_up) * step(0.001, wet_level);
		ALBEDO *= mix(1.0, 0.85, puddle);
		ROUGHNESS = mix(ROUGHNESS, 0.02, puddle);
		METALLIC = mix(METALLIC, 0.0, puddle);
)";
			if (features[FEATURE_NORMAL_MAPPING]) {
				code += "		NORMAL_MAP = mix(NORMAL_MAP, vec3(0.5, 0.5, 1.0), puddle);\n";
			}
		}
		code += "	}\n";
	}

	if (features[FEATURE_SNOW]) {
		// Mariner G4b: snow on what faces up - the normal with the normal map's detail (the up-facing sides of bricks
		// and planks hold it first), up to the steepest slope it rests on; with a height map, the low places fill
		// first. Under cover (OVERHEAD_COVER) none. Snow is white, rough, not metallic and smooths the detail.
		code += R"(
	// Snow: Enabled (Mariner G4b)
	{
		float snow_level = clamp(SNOW * snow_amount * (1.0 - OVERHEAD_COVER), 0.0, 1.0);
		vec3 snow_n = NORMAL;
)";
		if (features[FEATURE_NORMAL_MAPPING] && !impostor_hull) {
			code += R"(		vec3 snow_nm = NORMAL_MAP * 2.0 - 1.0;
		snow_nm.z = sqrt(max(0.0, 1.0 - dot(snow_nm.xy, snow_nm.xy)));
		snow_n = normalize(mix(NORMAL, TANGENT * snow_nm.x + BINORMAL * snow_nm.y + NORMAL * snow_nm.z, NORMAL_MAP_DEPTH));
)";
		}
		code += R"(		float snow_up = (INV_VIEW_MATRIX * vec4(snow_n, 0.0)).y;
		float snow_steepest = cos(radians(snow_max_slope));
		float snow_hold = smoothstep(snow_steepest, mix(snow_steepest, 1.0, 0.35), snow_up);
		float snow_depth = snow_level * snow_hold;
)";
		if (features[FEATURE_HEIGHT_MAPPING] && !flags[FLAG_UV1_USE_TRIPLANAR] && !impostor_hull) {
			code += flags[FLAG_INVERT_HEIGHTMAP] ? "		float snow_height = 1.0 - texture(texture_heightmap, base_uv).r;\n" : "		float snow_height = texture(texture_heightmap, base_uv).r;\n";
			code += "		snow_depth += (0.5 - snow_height) * snow_relief * snow_level * snow_hold;\n";
		}
		code += R"(		float snow_cover = smoothstep(0.05, 0.35, snow_depth);
		ALBEDO = mix(ALBEDO, snow_color.rgb, snow_cover);
		ROUGHNESS = mix(ROUGHNESS, snow_roughness, snow_cover);
		METALLIC = mix(METALLIC, 0.0, snow_cover);
)";
		if (features[FEATURE_NORMAL_MAPPING] && !impostor_hull) {
			code += "		NORMAL_MAP = mix(NORMAL_MAP, vec3(0.5, 0.5, 1.0), snow_cover * 0.7);\n";
		}
		code += "	}\n";
	}

	if (features[FEATURE_PLANAR_REFLECTION] && shading_mode != SHADING_MODE_UNSHADED) {
		// Mariner G6: the reflection camera's image is mirrored along X (its basis kept right-handed), so the mirror's
		// point of the screen is at (1 - u, v). Tilted by the normal map, blurred by the roughness (8 taps on a disc).
		code += R"(
	// Planar Reflection: Enabled (Mariner G6)
	{
		vec2 pr_uv = vec2(1.0 - SCREEN_UV.x, SCREEN_UV.y);
)";
		if (features[FEATURE_NORMAL_MAPPING]) {
			code += "		pr_uv += vec2(1.0, -1.0) * (NORMAL_MAP.xy * 2.0 - 1.0) * NORMAL_MAP_DEPTH * planar_reflection_distortion;\n";
		}
		code += R"(		float pr_radius = ROUGHNESS * ROUGHNESS * 0.08;
		vec3 pr_color = texture(texture_planar_reflection, pr_uv).rgb;
		if (pr_radius > 0.0005) {
			vec2 pr_aspect = vec2(1.0, VIEWPORT_SIZE.x / VIEWPORT_SIZE.y);
			for (int i = 0; i < 8; i++) {
				float pr_angle = float(i) * 2.39996;
				float pr_r = sqrt((float(i) + 0.5) / 8.0) * pr_radius;
				pr_color += texture(texture_planar_reflection, pr_uv + vec2(cos(pr_angle), sin(pr_angle)) * pr_r * pr_aspect).rgb;
			}
			pr_color /= 9.0;
		}
		REFLECTION = vec4(pr_color, planar_reflection_amount);
	}
)";
	}

	if (impostor_uv) {
		code += "\n	// Box impostor (Mariner G5b): the ray left the face or reached the bottom of the relief.\n	if (impostor_miss) {\n		discard;\n	}\n";
	}

	code += "}\n";

	// We must create the shader outside the shader_map_mutex to avoid potential deadlocks with
	// other tasks in the WorkerThreadPool simultaneously creating materials, which
	// may also hold the shared shader_map_mutex lock.
	RID new_shader = RS::get_singleton()->shader_create_from_code(code);

	MutexLock lock(shader_map_mutex);

	ShaderData *v = shader_map.getptr(mk);
	if (unlikely(v)) {
		// We raced and managed to create the same key concurrently, so we'll free the shader we just created,
		// given we know it isn't used, and use the winner.
		RS::get_singleton()->free_rid(new_shader);
	} else {
		ShaderData shader_data;
		shader_data.shader = new_shader;
		// ShaderData will be inserted with a users count of 0, but we
		// increment unconditionally outside this if block, whilst still under lock.
		v = &shader_map.insert(mk, shader_data)->value;
	}

	shader_rid = v->shader;
	v->users++;

	if (_get_material().is_valid()) {
		RS::get_singleton()->material_set_shader(_get_material(), shader_rid);
	}
}

void BaseMaterial3D::_check_material_rid() {
	MutexLock lock(material_rid_mutex);
	if (_get_material().is_null()) {
		RID next_pass_rid;
		if (get_next_pass().is_valid()) {
			next_pass_rid = get_next_pass()->get_rid();
		}

		_set_material(RS::get_singleton()->material_create_from_shader(next_pass_rid, get_render_priority(), shader_rid));

		for (KeyValue<StringName, Variant> param : pending_params) {
			RS::get_singleton()->material_set_param(_get_material(), param.key, param.value);
		}

		pending_params.clear();
	}
}

void BaseMaterial3D::flush_changes() {
	SelfList<BaseMaterial3D>::List copy;
	{
		MutexLock lock(material_mutex);
		while (SelfList<BaseMaterial3D> *E = dirty_materials.first()) {
			dirty_materials.remove(E);
			copy.add(E);
		}
	}

	while (SelfList<BaseMaterial3D> *E = copy.first()) {
		E->self()->_update_shader();
		copy.remove(E);
	}
}

void BaseMaterial3D::_queue_shader_change() {
	if (!_is_initialized()) {
		return;
	}

	MutexLock lock(material_mutex);

	if (!element.in_list()) {
		dirty_materials.add(&element);
	}
}

void BaseMaterial3D::_material_set_param(const StringName &p_name, const Variant &p_value) {
	if (_get_material().is_valid()) {
		RS::get_singleton()->material_set_param(_get_material(), p_name, p_value);
	} else {
		pending_params[p_name] = p_value;
	}
}

void BaseMaterial3D::set_albedo(const Color &p_albedo) {
	albedo = p_albedo;
	_material_set_param(shader_names->albedo, p_albedo);
}

Color BaseMaterial3D::get_albedo() const {
	return albedo;
}

void BaseMaterial3D::set_specular(float p_specular) {
	specular = p_specular;
	_material_set_param(shader_names->specular, p_specular);
}

float BaseMaterial3D::get_specular() const {
	return specular;
}

void BaseMaterial3D::set_roughness(float p_roughness) {
	roughness = p_roughness;
	_material_set_param(shader_names->roughness, p_roughness);
}

float BaseMaterial3D::get_roughness() const {
	return roughness;
}

void BaseMaterial3D::set_metallic(float p_metallic) {
	metallic = p_metallic;
	_material_set_param(shader_names->metallic, p_metallic);
}

float BaseMaterial3D::get_metallic() const {
	return metallic;
}

void BaseMaterial3D::set_emission(const Color &p_emission) {
	emission = p_emission;
	_material_set_param(shader_names->emission, p_emission);
}

Color BaseMaterial3D::get_emission() const {
	return emission;
}

void BaseMaterial3D::set_emission_energy_multiplier(float p_emission_energy_multiplier) {
	emission_energy_multiplier = p_emission_energy_multiplier;

	if (GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		_material_set_param(shader_names->emission_energy, p_emission_energy_multiplier * emission_intensity);
	} else {
		_material_set_param(shader_names->emission_energy, p_emission_energy_multiplier);
	}
}

float BaseMaterial3D::get_emission_energy_multiplier() const {
	return emission_energy_multiplier;
}

void BaseMaterial3D::set_emission_intensity(float p_emission_intensity) {
	ERR_FAIL_COND_EDMSG(!GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units"), "Cannot set material emission intensity when Physical Light Units disabled.");
	emission_intensity = p_emission_intensity;
	_material_set_param(shader_names->emission_energy, emission_energy_multiplier * emission_intensity);
}

float BaseMaterial3D::get_emission_intensity() const {
	return emission_intensity;
}

void BaseMaterial3D::set_normal_scale(float p_normal_scale) {
	normal_scale = p_normal_scale;
	_material_set_param(shader_names->normal_scale, p_normal_scale);
}

float BaseMaterial3D::get_normal_scale() const {
	return normal_scale;
}

void BaseMaterial3D::set_rim(float p_rim) {
	rim = p_rim;
	_material_set_param(shader_names->rim, p_rim);
}

float BaseMaterial3D::get_rim() const {
	return rim;
}

void BaseMaterial3D::set_rim_tint(float p_rim_tint) {
	rim_tint = p_rim_tint;
	_material_set_param(shader_names->rim_tint, p_rim_tint);
}

float BaseMaterial3D::get_rim_tint() const {
	return rim_tint;
}

void BaseMaterial3D::set_ao_light_affect(float p_ao_light_affect) {
	ao_light_affect = p_ao_light_affect;
	_material_set_param(shader_names->ao_light_affect, p_ao_light_affect);
}

float BaseMaterial3D::get_ao_light_affect() const {
	return ao_light_affect;
}

void BaseMaterial3D::set_clearcoat(float p_clearcoat) {
	clearcoat = p_clearcoat;
	_material_set_param(shader_names->clearcoat, p_clearcoat);
}

float BaseMaterial3D::get_clearcoat() const {
	return clearcoat;
}

void BaseMaterial3D::set_clearcoat_roughness(float p_clearcoat_roughness) {
	clearcoat_roughness = p_clearcoat_roughness;
	_material_set_param(shader_names->clearcoat_roughness, p_clearcoat_roughness);
}

float BaseMaterial3D::get_clearcoat_roughness() const {
	return clearcoat_roughness;
}

void BaseMaterial3D::set_anisotropy(float p_anisotropy) {
	anisotropy = p_anisotropy;
	_material_set_param(shader_names->anisotropy, p_anisotropy);
}

float BaseMaterial3D::get_anisotropy() const {
	return anisotropy;
}

void BaseMaterial3D::set_heightmap_scale(float p_heightmap_scale) {
	heightmap_scale = p_heightmap_scale;
	_material_set_param(shader_names->heightmap_scale, p_heightmap_scale);
}

float BaseMaterial3D::get_heightmap_scale() const {
	return heightmap_scale;
}

void BaseMaterial3D::set_subsurface_scattering_strength(float p_subsurface_scattering_strength) {
	subsurface_scattering_strength = p_subsurface_scattering_strength;
	_material_set_param(shader_names->subsurface_scattering_strength, subsurface_scattering_strength);
}

float BaseMaterial3D::get_subsurface_scattering_strength() const {
	return subsurface_scattering_strength;
}

void BaseMaterial3D::set_transmittance_color(const Color &p_color) {
	transmittance_color = p_color;
	_material_set_param(shader_names->transmittance_color, p_color);
}

Color BaseMaterial3D::get_transmittance_color() const {
	return transmittance_color;
}

void BaseMaterial3D::set_transmittance_depth(float p_depth) {
	transmittance_depth = p_depth;
	_material_set_param(shader_names->transmittance_depth, p_depth);
}

float BaseMaterial3D::get_transmittance_depth() const {
	return transmittance_depth;
}

void BaseMaterial3D::set_transmittance_boost(float p_boost) {
	transmittance_boost = p_boost;
	_material_set_param(shader_names->transmittance_boost, p_boost);
}

float BaseMaterial3D::get_transmittance_boost() const {
	return transmittance_boost;
}

void BaseMaterial3D::set_backlight(const Color &p_backlight) {
	backlight = p_backlight;
	_material_set_param(shader_names->backlight, backlight);
}

Color BaseMaterial3D::get_backlight() const {
	return backlight;
}

void BaseMaterial3D::set_refraction(float p_refraction) {
	refraction = p_refraction;
	_material_set_param(shader_names->refraction, refraction);
}

float BaseMaterial3D::get_refraction() const {
	return refraction;
}

void BaseMaterial3D::set_detail_uv(DetailUV p_detail_uv) {
	if (detail_uv == p_detail_uv) {
		return;
	}

	detail_uv = p_detail_uv;
	_queue_shader_change();
}

BaseMaterial3D::DetailUV BaseMaterial3D::get_detail_uv() const {
	return detail_uv;
}

void BaseMaterial3D::set_blend_mode(BlendMode p_mode) {
	if (blend_mode == p_mode) {
		return;
	}

	blend_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::BlendMode BaseMaterial3D::get_blend_mode() const {
	return blend_mode;
}

void BaseMaterial3D::set_detail_blend_mode(BlendMode p_mode) {
	detail_blend_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::BlendMode BaseMaterial3D::get_detail_blend_mode() const {
	return detail_blend_mode;
}

void BaseMaterial3D::set_transparency(Transparency p_transparency) {
	if (transparency == p_transparency) {
		return;
	}

	transparency = p_transparency;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::Transparency BaseMaterial3D::get_transparency() const {
	return transparency;
}

void BaseMaterial3D::set_alpha_mask_mode(AlphaMaskMode p_mode) {
	ERR_FAIL_INDEX(p_mode, ALPHA_MASK_MAX);
	if (alpha_mask_mode == p_mode) {
		return;
	}
	alpha_mask_mode = p_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::AlphaMaskMode BaseMaterial3D::get_alpha_mask_mode() const {
	return alpha_mask_mode;
}

Ref<Image> BaseMaterial3D::bake_alpha_mask(const Ref<Image> &p_source, const Vector2i &p_size, AlphaMaskMode p_mode, float p_range) {
	ERR_FAIL_COND_V_MSG(p_mode != ALPHA_MASK_SDF && p_mode != ALPHA_MASK_MSDF, Ref<Image>(), "Bake an SDF or an MSDF alpha mask.");
	return alpha_mask_bake(p_source, p_size, p_mode == ALPHA_MASK_MSDF, p_range);
}

void BaseMaterial3D::set_alpha_antialiasing(AlphaAntiAliasing p_alpha_aa) {
	if (alpha_antialiasing_mode == p_alpha_aa) {
		return;
	}

	alpha_antialiasing_mode = p_alpha_aa;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::AlphaAntiAliasing BaseMaterial3D::get_alpha_antialiasing() const {
	return alpha_antialiasing_mode;
}

void BaseMaterial3D::set_shading_mode(ShadingMode p_shading_mode) {
	if (shading_mode == p_shading_mode) {
		return;
	}

	shading_mode = p_shading_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::ShadingMode BaseMaterial3D::get_shading_mode() const {
	return shading_mode;
}

void BaseMaterial3D::set_depth_draw_mode(DepthDrawMode p_mode) {
	if (depth_draw_mode == p_mode) {
		return;
	}

	depth_draw_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::DepthDrawMode BaseMaterial3D::get_depth_draw_mode() const {
	return depth_draw_mode;
}

void BaseMaterial3D::set_depth_test(DepthTest p_func) {
	if (depth_test == p_func) {
		return;
	}

	depth_test = p_func;
	_queue_shader_change();
}

BaseMaterial3D::DepthTest BaseMaterial3D::get_depth_test() const {
	return depth_test;
}

void BaseMaterial3D::set_cull_mode(CullMode p_mode) {
	if (cull_mode == p_mode) {
		return;
	}

	cull_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::CullMode BaseMaterial3D::get_cull_mode() const {
	return cull_mode;
}

void BaseMaterial3D::set_diffuse_mode(DiffuseMode p_mode) {
	if (diffuse_mode == p_mode) {
		return;
	}

	diffuse_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::DiffuseMode BaseMaterial3D::get_diffuse_mode() const {
	return diffuse_mode;
}

void BaseMaterial3D::set_specular_mode(SpecularMode p_mode) {
	if (specular_mode == p_mode) {
		return;
	}

	specular_mode = p_mode;
	_queue_shader_change();
}

BaseMaterial3D::SpecularMode BaseMaterial3D::get_specular_mode() const {
	return specular_mode;
}

void BaseMaterial3D::set_flag(Flags p_flag, bool p_enabled) {
	ERR_FAIL_INDEX(p_flag, FLAG_MAX);

	if (flags[p_flag] == p_enabled) {
		return;
	}

	flags[p_flag] = p_enabled;

	if (
			p_flag == FLAG_USE_SHADOW_TO_OPACITY ||
			p_flag == FLAG_USE_TEXTURE_REPEAT ||
			p_flag == FLAG_SUBSURFACE_MODE_SKIN ||
			p_flag == FLAG_USE_POINT_SIZE ||
			p_flag == FLAG_UV1_USE_TRIPLANAR ||
			p_flag == FLAG_UV2_USE_TRIPLANAR ||
			p_flag == FLAG_USE_Z_CLIP_SCALE ||
			p_flag == FLAG_USE_FOV_OVERRIDE ||
			p_flag == FLAG_DISABLE_DEPTH_TEST) {
		notify_property_list_changed();
	}

	if (p_flag == FLAG_PARTICLE_TRAILS_MODE) {
		update_configuration_warning();
	}

	_queue_shader_change();
}

bool BaseMaterial3D::get_flag(Flags p_flag) const {
	ERR_FAIL_INDEX_V(p_flag, FLAG_MAX, false);
	return flags[p_flag];
}

void BaseMaterial3D::set_feature(Feature p_feature, bool p_enabled) {
	ERR_FAIL_INDEX(p_feature, FEATURE_MAX);
	if (features[p_feature] == p_enabled) {
		return;
	}

	features[p_feature] = p_enabled;
	_queue_shader_change();
}

bool BaseMaterial3D::get_feature(Feature p_feature) const {
	ERR_FAIL_INDEX_V(p_feature, FEATURE_MAX, false);
	return features[p_feature];
}

void BaseMaterial3D::set_texture(TextureParam p_param, const Ref<Texture2D> &p_texture) {
	ERR_FAIL_INDEX(p_param, TEXTURE_MAX);

	textures[p_param] = p_texture;
	Variant rid = p_texture.is_valid() ? Variant(p_texture->get_rid()) : Variant();
	_material_set_param(shader_names->texture_names[p_param], rid);

	if (p_texture.is_valid() && p_param == TEXTURE_ALBEDO) {
		_material_set_param(shader_names->albedo_texture_size, Vector2i(p_texture->get_width(), p_texture->get_height()));
	}

	notify_property_list_changed();
	_queue_shader_change();
}

Ref<Texture2D> BaseMaterial3D::get_texture(TextureParam p_param) const {
	ERR_FAIL_INDEX_V(p_param, TEXTURE_MAX, Ref<Texture2D>());
	return textures[p_param];
}

Ref<Texture2D> BaseMaterial3D::get_texture_by_name(const StringName &p_name) const {
	for (int i = 0; i < (int)BaseMaterial3D::TEXTURE_MAX; i++) {
		TextureParam param = TextureParam(i);
		if (p_name == shader_names->texture_names[param]) {
			return textures[param];
		}
	}
	return Ref<Texture2D>();
}

void BaseMaterial3D::set_texture_filter(TextureFilter p_filter) {
	texture_filter = p_filter;
	_queue_shader_change();
}

BaseMaterial3D::TextureFilter BaseMaterial3D::get_texture_filter() const {
	return texture_filter;
}

void BaseMaterial3D::_validate_property(PropertyInfo &p_property) const {
	if (p_property.name == "emission_intensity" && !GLOBAL_GET_CACHED(bool, "rendering/lights_and_shadows/use_physical_light_units")) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (p_property.name.begins_with("particles_anim_") && billboard_mode != BILLBOARD_PARTICLES) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (Engine::get_singleton()->is_editor_hint()) {
		if (p_property.name == "billboard_keep_scale" && billboard_mode == BILLBOARD_DISABLED) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "grow_amount" && !grow_enabled) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "point_size" && !flags[FLAG_USE_POINT_SIZE]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "proximity_fade_distance" && !proximity_fade_enabled) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "msdf_pixel_range" && !flags[FLAG_ALBEDO_TEXTURE_MSDF]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "msdf_outline_size" && !flags[FLAG_ALBEDO_TEXTURE_MSDF]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "distance_fade_max_distance" || p_property.name == "distance_fade_min_distance") && distance_fade == DISTANCE_FADE_DISABLED) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "uv1_triplanar_sharpness" || p_property.name == "uv1_world_triplanar") && !flags[FLAG_UV1_USE_TRIPLANAR]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "uv2_triplanar_sharpness" || p_property.name == "uv2_world_triplanar") && !flags[FLAG_UV2_USE_TRIPLANAR]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "z_clip_scale" && !flags[FLAG_USE_Z_CLIP_SCALE]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "fov_override" && !flags[FLAG_USE_FOV_OVERRIDE]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}

	// you can only enable anti-aliasing (in materials) on alpha scissor and alpha hash
	const bool can_select_aa = (transparency == TRANSPARENCY_ALPHA_SCISSOR || transparency == TRANSPARENCY_ALPHA_HASH);
	// alpha anti aliasiasing is only enabled when you can select aa
	const bool alpha_aa_enabled = (alpha_antialiasing_mode != ALPHA_ANTIALIASING_OFF) && (alpha_antialiasing_mode != ALPHA_ANTIALIASING_FRINGE) && can_select_aa;

	// alpha scissor slider isn't needed when alpha antialiasing is enabled
	if (p_property.name == "alpha_scissor_threshold" && transparency != TRANSPARENCY_ALPHA_SCISSOR) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	// alpha hash scale slider is only needed if transparency is alpha hash
	if (p_property.name == "alpha_hash_scale" && transparency != TRANSPARENCY_ALPHA_HASH) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (p_property.name == "alpha_antialiasing_mode" && !can_select_aa) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	// we can't choose an antialiasing mode if alpha isn't possible
	if (p_property.name == "wire_antialiasing" && transparency != TRANSPARENCY_ALPHA) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR; // Mariner AA3: needs alpha blending.
	}

	if (p_property.name == "alpha_antialiasing_edge" && !alpha_aa_enabled) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	// Mariner AA2: the alpha mask only with transparency, its texture only for a distance field.
	if ((p_property.name == "alpha_mask_mode" || p_property.name == "alpha_mask_texture") && transparency == TRANSPARENCY_DISABLED) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}
	if (p_property.name == "alpha_mask_texture" && alpha_mask_mode == ALPHA_MASK_ALBEDO) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}

	if (p_property.name == "blend_mode" && alpha_aa_enabled) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if ((p_property.name == "heightmap_min_layers" || p_property.name == "heightmap_max_layers" || p_property.name == "heightmap_impostor" || p_property.name == "heightmap_impostor_faces") && !deep_parallax) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (p_property.name == "heightmap_self_shadow_strength" && !heightmap_self_shadow) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}

	if ((p_property.name == "refraction_ior" || p_property.name == "refraction_thickness" || p_property.name.begins_with("refraction_absorption")) && !refraction_physical) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR; // Mariner F6.
	}
	if ((p_property.name == "refraction_scale" || p_property.name == "refraction_texture" || p_property.name == "refraction_texture_channel") && refraction_physical) {
		p_property.usage = PROPERTY_USAGE_NO_EDITOR;
	}

	if (Engine::get_singleton()->is_editor_hint()) {
		if (p_property.name == "depth_test" && flags[FLAG_DISABLE_DEPTH_TEST]) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "stencil_reference" && stencil_mode == STENCIL_MODE_DISABLED) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if ((p_property.name == "stencil_flags" || p_property.name == "stencil_compare") && stencil_mode != STENCIL_MODE_CUSTOM) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "stencil_color" && stencil_mode != STENCIL_MODE_OUTLINE && stencil_mode != STENCIL_MODE_XRAY) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}

		if (p_property.name == "stencil_outline_thickness" && stencil_mode != STENCIL_MODE_OUTLINE) {
			p_property.usage = PROPERTY_USAGE_NO_EDITOR;
		}
	}

	if (flags[FLAG_SUBSURFACE_MODE_SKIN] && (p_property.name == "subsurf_scatter_transmittance_color" || p_property.name == "subsurf_scatter_transmittance_texture")) {
		p_property.usage = PROPERTY_USAGE_NONE;
	}

	if (orm) {
		if (p_property.name == "shading_mode") {
			// Vertex not supported in ORM mode, since no individual roughness.
			p_property.hint_string = "Unshaded,Per-Pixel";
		}
		if (p_property.name.begins_with("roughness") || p_property.name.begins_with("metallic") || p_property.name.begins_with("ao_texture")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

	} else {
		if (p_property.name == "orm_texture") {
			p_property.usage = PROPERTY_USAGE_NONE;
		}
	}

	if (shading_mode != SHADING_MODE_PER_PIXEL) {
		if (shading_mode != SHADING_MODE_PER_VERTEX) {
			//these may still work per vertex
			if (p_property.name.begins_with("ao")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}
			if (p_property.name.begins_with("emission")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}

			if (p_property.name.begins_with("metallic")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}
			if (p_property.name.begins_with("rim")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}

			if (p_property.name.begins_with("roughness")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}

			if (p_property.name.begins_with("subsurf_scatter")) {
				p_property.usage = PROPERTY_USAGE_NONE;
			}
		}

		//these definitely only need per pixel
		if (p_property.name.begins_with("anisotropy")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("clearcoat")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("sheen") || p_property.name.begins_with("iridescence") || p_property.name.begins_with("wetness")) {
			p_property.usage = PROPERTY_USAGE_NONE; // Mariner M6, F5, G4 (snow stays: an unshaded surface is white with it too).
		}

		if (p_property.name.begins_with("normal")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("bent_normal")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("backlight")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}

		if (p_property.name.begins_with("transmittance")) {
			p_property.usage = PROPERTY_USAGE_NONE;
		}
	}
}

void BaseMaterial3D::set_point_size(float p_point_size) {
	point_size = p_point_size;
	_material_set_param(shader_names->point_size, p_point_size);
}

float BaseMaterial3D::get_point_size() const {
	return point_size;
}

void BaseMaterial3D::set_uv1_scale(const Vector3 &p_scale) {
	uv1_scale = p_scale;
	_material_set_param(shader_names->uv1_scale, p_scale);
}

Vector3 BaseMaterial3D::get_uv1_scale() const {
	return uv1_scale;
}

void BaseMaterial3D::set_uv1_offset(const Vector3 &p_offset) {
	uv1_offset = p_offset;
	_material_set_param(shader_names->uv1_offset, p_offset);
}

Vector3 BaseMaterial3D::get_uv1_offset() const {
	return uv1_offset;
}

void BaseMaterial3D::set_uv1_triplanar_blend_sharpness(float p_sharpness) {
	// Negative values or values higher than 150 can result in NaNs, leading to broken rendering.
	uv1_triplanar_sharpness = CLAMP(p_sharpness, 0.0, 150.0);
	_material_set_param(shader_names->uv1_blend_sharpness, uv1_triplanar_sharpness);
}

float BaseMaterial3D::get_uv1_triplanar_blend_sharpness() const {
	return uv1_triplanar_sharpness;
}

void BaseMaterial3D::set_uv2_scale(const Vector3 &p_scale) {
	uv2_scale = p_scale;
	_material_set_param(shader_names->uv2_scale, p_scale);
}

Vector3 BaseMaterial3D::get_uv2_scale() const {
	return uv2_scale;
}

void BaseMaterial3D::set_uv2_offset(const Vector3 &p_offset) {
	uv2_offset = p_offset;
	_material_set_param(shader_names->uv2_offset, p_offset);
}

Vector3 BaseMaterial3D::get_uv2_offset() const {
	return uv2_offset;
}

void BaseMaterial3D::set_uv2_triplanar_blend_sharpness(float p_sharpness) {
	// Negative values or values higher than 150 can result in NaNs, leading to broken rendering.
	uv2_triplanar_sharpness = CLAMP(p_sharpness, 0.0, 150.0);
	_material_set_param(shader_names->uv2_blend_sharpness, uv2_triplanar_sharpness);
}

float BaseMaterial3D::get_uv2_triplanar_blend_sharpness() const {
	return uv2_triplanar_sharpness;
}

void BaseMaterial3D::set_billboard_mode(BillboardMode p_mode) {
	billboard_mode = p_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::BillboardMode BaseMaterial3D::get_billboard_mode() const {
	return billboard_mode;
}

void BaseMaterial3D::set_particles_anim_h_frames(int p_frames) {
	particles_anim_h_frames = p_frames;
	_material_set_param(shader_names->particles_anim_h_frames, p_frames);
}

int BaseMaterial3D::get_particles_anim_h_frames() const {
	return particles_anim_h_frames;
}

void BaseMaterial3D::set_particles_anim_v_frames(int p_frames) {
	particles_anim_v_frames = p_frames;
	_material_set_param(shader_names->particles_anim_v_frames, p_frames);
}

int BaseMaterial3D::get_particles_anim_v_frames() const {
	return particles_anim_v_frames;
}

void BaseMaterial3D::set_particles_anim_loop(bool p_loop) {
	particles_anim_loop = p_loop;
	_material_set_param(shader_names->particles_anim_loop, particles_anim_loop);
}

bool BaseMaterial3D::get_particles_anim_loop() const {
	return particles_anim_loop;
}

void BaseMaterial3D::set_heightmap_self_shadow(bool p_enable) {
	heightmap_self_shadow = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_heightmap_self_shadow_enabled() const {
	return heightmap_self_shadow;
}

void BaseMaterial3D::set_heightmap_self_shadow_strength(float p_strength) {
	heightmap_self_shadow_strength = CLAMP(p_strength, 0.0f, 1.0f);
	_material_set_param(shader_names->heightmap_self_shadow_strength, heightmap_self_shadow_strength);
}

float BaseMaterial3D::get_heightmap_self_shadow_strength() const {
	return heightmap_self_shadow_strength;
}

// Mariner M6, F5, F6, M4.

void BaseMaterial3D::set_sheen_color(const Color &p_value) {
	sheen_color = p_value;
	_material_set_param(shader_names->sheen_color, sheen_color);
}

// Mariner G4.
void BaseMaterial3D::set_wetness_amount(float p_value) {
	wetness_amount = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->wetness_amount, wetness_amount);
}

float BaseMaterial3D::get_wetness_amount() const {
	return wetness_amount;
}

void BaseMaterial3D::set_wetness_porosity(float p_value) {
	wetness_porosity = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->wetness_porosity, wetness_porosity);
}

float BaseMaterial3D::get_wetness_porosity() const {
	return wetness_porosity;
}

void BaseMaterial3D::set_wetness_puddles(float p_value) {
	wetness_puddles = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->wetness_puddles, wetness_puddles);
}

float BaseMaterial3D::get_wetness_puddles() const {
	return wetness_puddles;
}

void BaseMaterial3D::set_snow_amount(float p_value) {
	snow_amount = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->snow_amount, snow_amount);
}

float BaseMaterial3D::get_snow_amount() const {
	return snow_amount;
}

void BaseMaterial3D::set_snow_color(const Color &p_color) {
	snow_color = p_color;
	_material_set_param(shader_names->snow_color, snow_color);
}

Color BaseMaterial3D::get_snow_color() const {
	return snow_color;
}

void BaseMaterial3D::set_snow_roughness(float p_value) {
	snow_roughness = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->snow_roughness, snow_roughness);
}

float BaseMaterial3D::get_snow_roughness() const {
	return snow_roughness;
}

void BaseMaterial3D::set_snow_max_slope(float p_degrees) {
	snow_max_slope = CLAMP(p_degrees, 0.0f, 90.0f);
	_material_set_param(shader_names->snow_max_slope, snow_max_slope);
}

float BaseMaterial3D::get_snow_max_slope() const {
	return snow_max_slope;
}

void BaseMaterial3D::set_snow_relief(float p_value) {
	snow_relief = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->snow_relief, snow_relief);
}

float BaseMaterial3D::get_snow_relief() const {
	return snow_relief;
}

// Mariner G5a.
void BaseMaterial3D::set_interior_rooms(const Vector2 &p_value) {
	interior_rooms = p_value.maxf(0.001);
	_material_set_param(shader_names->interior_rooms, interior_rooms);
}

Vector2 BaseMaterial3D::get_interior_rooms() const {
	return interior_rooms;
}

void BaseMaterial3D::set_interior_room_size(const Vector3 &p_value) {
	interior_room_size = p_value.maxf(0.01);
	_material_set_param(shader_names->interior_room_size, interior_room_size);
}

Vector3 BaseMaterial3D::get_interior_room_size() const {
	return interior_room_size;
}

void BaseMaterial3D::set_interior_atlas_size(const Vector2i &p_value) {
	interior_atlas_size = p_value.maxi(1);
	_material_set_param(shader_names->interior_atlas_size, interior_atlas_size);
}

Vector2i BaseMaterial3D::get_interior_atlas_size() const {
	return interior_atlas_size;
}

void BaseMaterial3D::set_interior_back_wall(float p_value) {
	interior_back_wall = CLAMP(p_value, 0.05f, 0.95f);
	_material_set_param(shader_names->interior_back_wall, interior_back_wall);
}

float BaseMaterial3D::get_interior_back_wall() const {
	return interior_back_wall;
}

void BaseMaterial3D::set_interior_brightness(float p_value) {
	interior_brightness = MAX(p_value, 0.0f);
	_material_set_param(shader_names->interior_brightness, interior_brightness);
}

float BaseMaterial3D::get_interior_brightness() const {
	return interior_brightness;
}

void BaseMaterial3D::set_interior_lights(float p_value) {
	interior_lights = MAX(p_value, 0.0f);
	_material_set_param(shader_names->interior_lights, interior_lights);
}

float BaseMaterial3D::get_interior_lights() const {
	return interior_lights;
}

void BaseMaterial3D::set_interior_lit_fraction(float p_value) {
	interior_lit_fraction = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->interior_lit_fraction, interior_lit_fraction);
}

float BaseMaterial3D::get_interior_lit_fraction() const {
	return interior_lit_fraction;
}

void BaseMaterial3D::set_interior_curtains(float p_value) {
	interior_curtains = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->interior_curtains, interior_curtains);
}

float BaseMaterial3D::get_interior_curtains() const {
	return interior_curtains;
}

void BaseMaterial3D::set_interior_blinds(float p_value) {
	interior_blinds = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->interior_blinds, interior_blinds);
}

float BaseMaterial3D::get_interior_blinds() const {
	return interior_blinds;
}

void BaseMaterial3D::set_interior_curtain_color(const Color &p_value) {
	interior_curtain_color = p_value;
	_material_set_param(shader_names->interior_curtain_color, interior_curtain_color);
}

Color BaseMaterial3D::get_interior_curtain_color() const {
	return interior_curtain_color;
}

void BaseMaterial3D::set_interior_light_color(const Color &p_value) {
	interior_light_color = p_value;
	_material_set_param(shader_names->interior_light_color, interior_light_color);
}

Color BaseMaterial3D::get_interior_light_color() const {
	return interior_light_color;
}

Color BaseMaterial3D::get_sheen_color() const {
	return sheen_color;
}

void BaseMaterial3D::set_sheen_roughness(float p_value) {
	sheen_roughness = CLAMP(p_value, float(0), float(1));
	_material_set_param(shader_names->sheen_roughness, sheen_roughness);
}

float BaseMaterial3D::get_sheen_roughness() const {
	return sheen_roughness;
}

void BaseMaterial3D::set_iridescence(float p_value) {
	iridescence = CLAMP(p_value, float(0), float(1));
	_material_set_param(shader_names->iridescence, iridescence);
}

float BaseMaterial3D::get_iridescence() const {
	return iridescence;
}

void BaseMaterial3D::set_iridescence_ior(float p_value) {
	iridescence_ior = CLAMP(p_value, float(1.0), float(2.333));
	_material_set_param(shader_names->iridescence_ior, iridescence_ior);
}

float BaseMaterial3D::get_iridescence_ior() const {
	return iridescence_ior;
}

void BaseMaterial3D::set_iridescence_thickness_min(float p_value) {
	iridescence_thickness_min = CLAMP(p_value, float(0), float(1200));
	_material_set_param(shader_names->iridescence_thickness_min, iridescence_thickness_min);
}

float BaseMaterial3D::get_iridescence_thickness_min() const {
	return iridescence_thickness_min;
}

void BaseMaterial3D::set_iridescence_thickness_max(float p_value) {
	iridescence_thickness_max = CLAMP(p_value, float(0), float(1200));
	_material_set_param(shader_names->iridescence_thickness_max, iridescence_thickness_max);
}

float BaseMaterial3D::get_iridescence_thickness_max() const {
	return iridescence_thickness_max;
}

void BaseMaterial3D::set_refraction_ior(float p_value) {
	refraction_ior = CLAMP(p_value, float(1.0), float(3.0));
	_material_set_param(shader_names->refraction_ior, refraction_ior);
}

float BaseMaterial3D::get_refraction_ior() const {
	return refraction_ior;
}

void BaseMaterial3D::set_refraction_thickness(float p_value) {
	refraction_thickness = CLAMP(p_value, float(0), float(1000));
	_material_set_param(shader_names->refraction_thickness, refraction_thickness);
}

float BaseMaterial3D::get_refraction_thickness() const {
	return refraction_thickness;
}

void BaseMaterial3D::set_refraction_absorption_color(const Color &p_value) {
	refraction_absorption_color = p_value;
	_material_set_param(shader_names->refraction_absorption_color, refraction_absorption_color);
}

Color BaseMaterial3D::get_refraction_absorption_color() const {
	return refraction_absorption_color;
}

void BaseMaterial3D::set_refraction_absorption_distance(float p_value) {
	refraction_absorption_distance = CLAMP(p_value, float(0.001), float(100000));
	_material_set_param(shader_names->refraction_absorption_distance, refraction_absorption_distance);
}

float BaseMaterial3D::get_refraction_absorption_distance() const {
	return refraction_absorption_distance;
}

void BaseMaterial3D::set_refraction_physical(bool p_enable) {
	refraction_physical = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_refraction_physical() const {
	return refraction_physical;
}

void BaseMaterial3D::set_displacement_scale(float p_value) {
	displacement_scale = p_value;
	_material_set_param(shader_names->displacement_scale, displacement_scale);
}

float BaseMaterial3D::get_displacement_scale() const {
	return displacement_scale;
}

void BaseMaterial3D::set_displacement_midlevel(float p_value) {
	displacement_midlevel = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->displacement_midlevel, displacement_midlevel);
}

float BaseMaterial3D::get_displacement_midlevel() const {
	return displacement_midlevel;
}

void BaseMaterial3D::set_displacement_texture_lod(float p_value) {
	displacement_texture_lod = MAX(p_value, 0.0f);
	_material_set_param(shader_names->displacement_texture_lod, displacement_texture_lod);
}

float BaseMaterial3D::get_displacement_texture_lod() const {
	return displacement_texture_lod;
}

void BaseMaterial3D::set_displacement_normal(bool p_enable) {
	displacement_normal = p_enable;
	_queue_shader_change();
}

bool BaseMaterial3D::is_displacement_normal_enabled() const {
	return displacement_normal;
}

void BaseMaterial3D::set_detail_normal_rnm(bool p_enable) {
	detail_normal_rnm = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_detail_normal_rnm() const {
	return detail_normal_rnm;
}

// Mariner M8: F0 of pure metals in linear Rec. 709, from measured spectral optical constants (as tabulated in
// "Real-Time Rendering", 4th ed., table 9.2). Returned in sRGB, as albedo_color takes it (metallic = 1). Steel is
// close to iron; oxidized or painted metal is not a metal for shading.
Color BaseMaterial3D::get_metal_albedo(Metal p_metal) {
	static const Color f0[METAL_MAX] = {
		Color(0.913, 0.922, 0.924), // Aluminum
		Color(0.910, 0.778, 0.423), // Brass
		Color(0.549, 0.556, 0.554), // Chromium
		Color(0.955, 0.638, 0.538), // Copper
		Color(1.000, 0.782, 0.344), // Gold
		Color(0.562, 0.565, 0.578), // Iron
		Color(0.781, 0.780, 0.778), // Mercury
		Color(0.660, 0.609, 0.526), // Nickel
		Color(0.733, 0.697, 0.652), // Palladium
		Color(0.673, 0.637, 0.585), // Platinum
		Color(0.972, 0.960, 0.915), // Silver
		Color(0.542, 0.497, 0.449), // Titanium
		Color(0.664, 0.824, 0.850), // Zinc
	};
	ERR_FAIL_INDEX_V(p_metal, METAL_MAX, Color(1, 1, 1));
	return f0[p_metal].linear_to_srgb();
}

void BaseMaterial3D::set_heightmap_depth_offset(bool p_enable) {
	heightmap_depth_offset = p_enable;
	_queue_shader_change();
}

bool BaseMaterial3D::is_heightmap_depth_offset_enabled() const {
	return heightmap_depth_offset;
}

// Mariner G5b.
void BaseMaterial3D::set_heightmap_impostor(bool p_enable) {
	heightmap_impostor = p_enable;
	_queue_shader_change();
}

bool BaseMaterial3D::is_heightmap_impostor_enabled() const {
	return heightmap_impostor;
}

// Mariner G5c.
void BaseMaterial3D::set_heightmap_impostor_faces(const PackedVector4Array &p_faces) {
	const bool hull_changed = (heightmap_impostor_faces.size() >= IMPOSTOR_FACE_DATA_SIZE) != (p_faces.size() >= IMPOSTOR_FACE_DATA_SIZE);
	heightmap_impostor_faces = p_faces;
	if (p_faces.size() >= IMPOSTOR_FACE_DATA_SIZE) {
		_material_set_param(shader_names->impostor_faces, p_faces.slice(0, IMPOSTOR_FACE_DATA_SIZE));
	}
	if (hull_changed) {
		_queue_shader_change();
	}
}

PackedVector4Array BaseMaterial3D::get_heightmap_impostor_faces() const {
	return heightmap_impostor_faces;
}

// Mariner G6.
void BaseMaterial3D::set_planar_reflection_amount(float p_value) {
	planar_reflection_amount = CLAMP(p_value, 0.0f, 1.0f);
	_material_set_param(shader_names->planar_reflection_amount, planar_reflection_amount);
}

float BaseMaterial3D::get_planar_reflection_amount() const {
	return planar_reflection_amount;
}

void BaseMaterial3D::set_planar_reflection_distortion(float p_value) {
	planar_reflection_distortion = MAX(p_value, 0.0f);
	_material_set_param(shader_names->planar_reflection_distortion, planar_reflection_distortion);
}

float BaseMaterial3D::get_planar_reflection_distortion() const {
	return planar_reflection_distortion;
}

void BaseMaterial3D::set_heightmap_deep_parallax(bool p_enable) {
	deep_parallax = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_heightmap_deep_parallax_enabled() const {
	return deep_parallax;
}

void BaseMaterial3D::set_heightmap_deep_parallax_min_layers(int p_layer) {
	deep_parallax_min_layers = p_layer;
	_material_set_param(shader_names->heightmap_min_layers, p_layer);
}

int BaseMaterial3D::get_heightmap_deep_parallax_min_layers() const {
	return deep_parallax_min_layers;
}

void BaseMaterial3D::set_heightmap_deep_parallax_max_layers(int p_layer) {
	deep_parallax_max_layers = p_layer;
	_material_set_param(shader_names->heightmap_max_layers, p_layer);
}

int BaseMaterial3D::get_heightmap_deep_parallax_max_layers() const {
	return deep_parallax_max_layers;
}

void BaseMaterial3D::set_heightmap_deep_parallax_flip_tangent(bool p_flip) {
	heightmap_parallax_flip_tangent = p_flip;
	_material_set_param(shader_names->heightmap_flip, Vector2(heightmap_parallax_flip_tangent ? -1 : 1, heightmap_parallax_flip_binormal ? -1 : 1));
}

bool BaseMaterial3D::get_heightmap_deep_parallax_flip_tangent() const {
	return heightmap_parallax_flip_tangent;
}

void BaseMaterial3D::set_heightmap_deep_parallax_flip_binormal(bool p_flip) {
	heightmap_parallax_flip_binormal = p_flip;
	_material_set_param(shader_names->heightmap_flip, Vector2(heightmap_parallax_flip_tangent ? -1 : 1, heightmap_parallax_flip_binormal ? -1 : 1));
}

bool BaseMaterial3D::get_heightmap_deep_parallax_flip_binormal() const {
	return heightmap_parallax_flip_binormal;
}

void BaseMaterial3D::set_wire_antialiasing(bool p_enable) {
	if (wire_antialiasing == p_enable) {
		return;
	}
	wire_antialiasing = p_enable;
	_queue_shader_change();
}

bool BaseMaterial3D::is_wire_antialiasing() const {
	return wire_antialiasing;
}

void BaseMaterial3D::set_grow_enabled(bool p_enable) {
	grow_enabled = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_grow_enabled() const {
	return grow_enabled;
}

void BaseMaterial3D::set_alpha_scissor_threshold(float p_threshold) {
	alpha_scissor_threshold = p_threshold;
	_material_set_param(shader_names->alpha_scissor_threshold, p_threshold);
}

float BaseMaterial3D::get_alpha_scissor_threshold() const {
	return alpha_scissor_threshold;
}

void BaseMaterial3D::set_alpha_hash_scale(float p_scale) {
	alpha_hash_scale = p_scale;
	_material_set_param(shader_names->alpha_hash_scale, p_scale);
}

float BaseMaterial3D::get_alpha_hash_scale() const {
	return alpha_hash_scale;
}

void BaseMaterial3D::set_alpha_antialiasing_edge(float p_edge) {
	alpha_antialiasing_edge = p_edge;
	_material_set_param(shader_names->alpha_antialiasing_edge, p_edge);
}

float BaseMaterial3D::get_alpha_antialiasing_edge() const {
	return alpha_antialiasing_edge;
}

void BaseMaterial3D::set_grow(float p_grow) {
	grow = p_grow;
	_material_set_param(shader_names->grow, p_grow);
}

float BaseMaterial3D::get_grow() const {
	return grow;
}

static Vector4 _get_texture_mask(BaseMaterial3D::TextureChannel p_channel) {
	static const Vector4 masks[5] = {
		Vector4(1, 0, 0, 0),
		Vector4(0, 1, 0, 0),
		Vector4(0, 0, 1, 0),
		Vector4(0, 0, 0, 1),
		Vector4(0.3333333, 0.3333333, 0.3333333, 0),
	};

	return masks[p_channel];
}

void BaseMaterial3D::set_metallic_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	metallic_texture_channel = p_channel;
	_material_set_param(shader_names->metallic_texture_channel, _get_texture_mask(p_channel));
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_metallic_texture_channel() const {
	return metallic_texture_channel;
}

void BaseMaterial3D::set_roughness_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	roughness_texture_channel = p_channel;
	_queue_shader_change();
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_roughness_texture_channel() const {
	return roughness_texture_channel;
}

void BaseMaterial3D::set_ao_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	ao_texture_channel = p_channel;
	_material_set_param(shader_names->ao_texture_channel, _get_texture_mask(p_channel));
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_ao_texture_channel() const {
	return ao_texture_channel;
}

void BaseMaterial3D::set_refraction_texture_channel(TextureChannel p_channel) {
	ERR_FAIL_INDEX(p_channel, 5);
	refraction_texture_channel = p_channel;
	_material_set_param(shader_names->refraction_texture_channel, _get_texture_mask(p_channel));
}

BaseMaterial3D::TextureChannel BaseMaterial3D::get_refraction_texture_channel() const {
	return refraction_texture_channel;
}

void BaseMaterial3D::set_z_clip_scale(float p_z_clip_scale) {
	z_clip_scale = p_z_clip_scale;
	_material_set_param(shader_names->z_clip_scale, p_z_clip_scale);
}

float BaseMaterial3D::get_z_clip_scale() const {
	return z_clip_scale;
}

void BaseMaterial3D::set_fov_override(float p_fov_override) {
	fov_override = p_fov_override;
	_material_set_param(shader_names->fov_override, p_fov_override);
}

float BaseMaterial3D::get_fov_override() const {
	return fov_override;
}

Ref<Material> BaseMaterial3D::get_material_for_2d(bool p_shaded, Transparency p_transparency, bool p_double_sided, bool p_billboard, bool p_billboard_y, bool p_msdf, bool p_no_depth, bool p_fixed_size, TextureFilter p_filter, AlphaAntiAliasing p_alpha_antialiasing_mode, bool p_texture_repeat, RID *r_shader_rid) {
	uint64_t key = 0;
	key |= ((int8_t)p_shaded & 0x01) << 0;
	key |= ((int8_t)p_transparency & 0x07) << 1; // Bits 1-3.
	key |= ((int8_t)p_double_sided & 0x01) << 4;
	key |= ((int8_t)p_billboard & 0x01) << 5;
	key |= ((int8_t)p_billboard_y & 0x01) << 6;
	key |= ((int8_t)p_msdf & 0x01) << 7;
	key |= ((int8_t)p_no_depth & 0x01) << 8;
	key |= ((int8_t)p_fixed_size & 0x01) << 9;
	key |= ((int8_t)p_filter & 0x07) << 10; // Bits 10-12.
	key |= ((int8_t)p_alpha_antialiasing_mode & 0x07) << 13; // Bits 13-15.
	key |= ((int8_t)p_texture_repeat & 0x01) << 16;

	if (materials_for_2d.has(key)) {
		if (r_shader_rid) {
			*r_shader_rid = materials_for_2d[key]->get_shader_rid();
		}
		return materials_for_2d[key];
	}

	Ref<StandardMaterial3D> material;
	material.instantiate();

	material->set_shading_mode(p_shaded ? SHADING_MODE_PER_PIXEL : SHADING_MODE_UNSHADED);
	material->set_transparency(p_transparency);
	material->set_cull_mode(p_double_sided ? CULL_DISABLED : CULL_BACK);
	material->set_flag(FLAG_SRGB_VERTEX_COLOR, true);
	material->set_flag(FLAG_ALBEDO_FROM_VERTEX_COLOR, true);
	material->set_flag(FLAG_ALBEDO_TEXTURE_MSDF, p_msdf);
	material->set_flag(FLAG_DISABLE_DEPTH_TEST, p_no_depth);
	material->set_flag(FLAG_FIXED_SIZE, p_fixed_size);
	material->set_flag(FLAG_USE_TEXTURE_REPEAT, p_texture_repeat);
	material->set_alpha_antialiasing(p_alpha_antialiasing_mode);
	material->set_texture_filter(p_filter);
	if (p_billboard || p_billboard_y) {
		material->set_flag(FLAG_BILLBOARD_KEEP_SCALE, true);
		material->set_billboard_mode(p_billboard_y ? BILLBOARD_FIXED_Y : BILLBOARD_ENABLED);
	}

	materials_for_2d[key] = material;

	if (r_shader_rid) {
		*r_shader_rid = materials_for_2d[key]->get_shader_rid();
	}

	return materials_for_2d[key];
}

void BaseMaterial3D::set_on_top_of_alpha() {
	set_transparency(TRANSPARENCY_DISABLED);
	set_render_priority(RENDER_PRIORITY_MAX);
	set_flag(FLAG_DISABLE_DEPTH_TEST, true);
}

void BaseMaterial3D::set_proximity_fade_enabled(bool p_enable) {
	proximity_fade_enabled = p_enable;
	_queue_shader_change();
	notify_property_list_changed();
}

bool BaseMaterial3D::is_proximity_fade_enabled() const {
	return proximity_fade_enabled;
}

void BaseMaterial3D::set_proximity_fade_distance(float p_distance) {
	proximity_fade_distance = MAX(p_distance, 0.01);
	_material_set_param(shader_names->proximity_fade_distance, proximity_fade_distance);
}

float BaseMaterial3D::get_proximity_fade_distance() const {
	return proximity_fade_distance;
}

void BaseMaterial3D::set_msdf_pixel_range(float p_range) {
	msdf_pixel_range = p_range;
	_material_set_param(shader_names->msdf_pixel_range, p_range);
}

float BaseMaterial3D::get_msdf_pixel_range() const {
	return msdf_pixel_range;
}

void BaseMaterial3D::set_msdf_outline_size(float p_size) {
	msdf_outline_size = p_size;
	_material_set_param(shader_names->msdf_outline_size, p_size);
}

float BaseMaterial3D::get_msdf_outline_size() const {
	return msdf_outline_size;
}

void BaseMaterial3D::set_distance_fade(DistanceFadeMode p_mode) {
	distance_fade = p_mode;
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::DistanceFadeMode BaseMaterial3D::get_distance_fade() const {
	return distance_fade;
}

void BaseMaterial3D::set_distance_fade_max_distance(float p_distance) {
	distance_fade_max_distance = p_distance;
	_material_set_param(shader_names->distance_fade_max, distance_fade_max_distance);
}

float BaseMaterial3D::get_distance_fade_max_distance() const {
	return distance_fade_max_distance;
}

void BaseMaterial3D::set_distance_fade_min_distance(float p_distance) {
	distance_fade_min_distance = p_distance;
	_material_set_param(shader_names->distance_fade_min, distance_fade_min_distance);
}

float BaseMaterial3D::get_distance_fade_min_distance() const {
	return distance_fade_min_distance;
}

void BaseMaterial3D::set_emission_operator(EmissionOperator p_op) {
	if (emission_op == p_op) {
		return;
	}
	emission_op = p_op;
	_queue_shader_change();
}

BaseMaterial3D::EmissionOperator BaseMaterial3D::get_emission_operator() const {
	return emission_op;
}

RID BaseMaterial3D::get_rid() const {
	const_cast<BaseMaterial3D *>(this)->_update_shader();
	const_cast<BaseMaterial3D *>(this)->_check_material_rid();
	return _get_material();
}

void BaseMaterial3D::_prepare_stencil_effect() {
	const Ref<Material> current_next_pass = get_next_pass();

	if (stencil_mode == STENCIL_MODE_DISABLED || stencil_mode == STENCIL_MODE_CUSTOM) {
		if (current_next_pass.is_valid() && current_next_pass->has_meta("_stencil_owned")) {
			set_next_pass(current_next_pass->get_next_pass());
		}
		return;
	}

	Ref<BaseMaterial3D> stencil_next_pass;

	if (current_next_pass.is_null() || !current_next_pass->has_meta("_stencil_owned")) {
		stencil_next_pass = Ref<BaseMaterial3D>(memnew(StandardMaterial3D));
		stencil_next_pass->set_meta("_stencil_owned", true);
		stencil_next_pass->set_next_pass(current_next_pass);
		set_next_pass(stencil_next_pass);
	} else {
		stencil_next_pass = current_next_pass;
	}

	switch (stencil_mode) {
		case STENCIL_MODE_DISABLED:
			break;
		case STENCIL_MODE_OUTLINE:
			set_stencil_flags(STENCIL_FLAG_WRITE);
			set_stencil_compare(STENCIL_COMPARE_ALWAYS);
			stencil_next_pass->set_render_priority(get_render_priority() + 1);
			stencil_next_pass->set_shading_mode(SHADING_MODE_UNSHADED);
			stencil_next_pass->set_transparency(TRANSPARENCY_ALPHA);
			stencil_next_pass->set_flag(FLAG_DISABLE_DEPTH_TEST, false);
			stencil_next_pass->set_grow_enabled(true);
			stencil_next_pass->set_grow(stencil_effect_outline_thickness);
			stencil_next_pass->set_albedo(stencil_effect_color);
			stencil_next_pass->set_stencil_mode(STENCIL_MODE_CUSTOM);
			stencil_next_pass->set_stencil_flags(STENCIL_FLAG_READ);
			stencil_next_pass->set_stencil_compare(STENCIL_COMPARE_NOT_EQUAL);
			stencil_next_pass->set_stencil_reference(stencil_reference);
			break;
		case STENCIL_MODE_XRAY:
			set_stencil_flags(STENCIL_FLAG_WRITE);
			set_stencil_compare(STENCIL_COMPARE_ALWAYS);
			stencil_next_pass->set_render_priority(get_render_priority() + 1);
			stencil_next_pass->set_shading_mode(SHADING_MODE_UNSHADED);
			stencil_next_pass->set_transparency(TRANSPARENCY_ALPHA);
			stencil_next_pass->set_flag(FLAG_DISABLE_DEPTH_TEST, true);
			stencil_next_pass->set_grow_enabled(false);
			stencil_next_pass->set_grow(0);
			stencil_next_pass->set_albedo(stencil_effect_color);
			stencil_next_pass->set_stencil_mode(STENCIL_MODE_CUSTOM);
			stencil_next_pass->set_stencil_flags(STENCIL_FLAG_READ);
			stencil_next_pass->set_stencil_compare(STENCIL_COMPARE_NOT_EQUAL);
			stencil_next_pass->set_stencil_reference(stencil_reference);
			break;
		case STENCIL_MODE_CUSTOM:
			break;
		case STENCIL_MODE_MAX:
			break;
	}
}

Ref<BaseMaterial3D> BaseMaterial3D::_get_stencil_next_pass() const {
	const Ref<Material> current_next_pass = get_next_pass();
	Ref<BaseMaterial3D> stencil_next_pass;

	if (current_next_pass.is_valid() && current_next_pass->has_meta("_stencil_owned")) {
		stencil_next_pass = current_next_pass;
	}

	return stencil_next_pass;
}

void BaseMaterial3D::set_stencil_mode(StencilMode p_stencil_mode) {
	if (stencil_mode == p_stencil_mode) {
		return;
	}

	if (p_stencil_mode == StencilMode::STENCIL_MODE_OUTLINE || p_stencil_mode == StencilMode::STENCIL_MODE_XRAY) {
		ERR_FAIL_COND_EDMSG(get_render_priority() >= RENDER_PRIORITY_MAX,
				vformat("Cannot use stencil mode Outline or Xray, when render priority is RENDER_PRIORITY_MAX(%d).", RENDER_PRIORITY_MAX));
	}

	stencil_mode = p_stencil_mode;
	_prepare_stencil_effect();
	_queue_shader_change();
	notify_property_list_changed();
}

BaseMaterial3D::StencilMode BaseMaterial3D::get_stencil_mode() const {
	return stencil_mode;
}

void BaseMaterial3D::set_stencil_flags(int p_stencil_flags) {
	if (stencil_flags == p_stencil_flags) {
		return;
	}

	// If enabling read while already writing, switch to read only.
	if ((p_stencil_flags & STENCIL_FLAG_READ) && (stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL))) {
		p_stencil_flags = p_stencil_flags & STENCIL_FLAG_READ;
	}

	// If enabling write while already reading, switch to write or write_depth_fail.
	if ((p_stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL)) && (stencil_flags & STENCIL_FLAG_READ)) {
		p_stencil_flags = p_stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL);
	}

	// If enabling read+write while already doing neither, only allow read.
	if ((p_stencil_flags & STENCIL_FLAG_READ) && (p_stencil_flags & (STENCIL_FLAG_WRITE | STENCIL_FLAG_WRITE_DEPTH_FAIL))) {
		p_stencil_flags = p_stencil_flags & STENCIL_FLAG_READ;
	}

	stencil_flags = p_stencil_flags;
	_queue_shader_change();
}

int BaseMaterial3D::get_stencil_flags() const {
	return stencil_flags;
}

void BaseMaterial3D::set_stencil_compare(BaseMaterial3D::StencilCompare p_op) {
	if (stencil_compare == p_op) {
		return;
	}

	stencil_compare = p_op;
	_queue_shader_change();
}

BaseMaterial3D::StencilCompare BaseMaterial3D::get_stencil_compare() const {
	return stencil_compare;
}

void BaseMaterial3D::set_stencil_reference(int p_reference) {
	if (stencil_reference == p_reference) {
		return;
	}

	stencil_reference = p_reference;
	_queue_shader_change();

	Ref<BaseMaterial3D> stencil_next_pass = _get_stencil_next_pass();
	if (stencil_next_pass.is_valid()) {
		stencil_next_pass->set_stencil_reference(p_reference);
	}
}

int BaseMaterial3D::get_stencil_reference() const {
	return stencil_reference;
}

void BaseMaterial3D::set_stencil_effect_color(const Color &p_color) {
	if (stencil_effect_color == p_color) {
		return;
	}

	stencil_effect_color = p_color;

	Ref<BaseMaterial3D> stencil_next_pass = _get_stencil_next_pass();
	if (stencil_next_pass.is_valid()) {
		stencil_next_pass->set_albedo(p_color);
	}
}

Color BaseMaterial3D::get_stencil_effect_color() const {
	return stencil_effect_color;
}

void BaseMaterial3D::set_stencil_effect_outline_thickness(float p_outline_thickness) {
	if (stencil_effect_outline_thickness == p_outline_thickness) {
		return;
	}

	stencil_effect_outline_thickness = p_outline_thickness;

	Ref<BaseMaterial3D> stencil_next_pass = _get_stencil_next_pass();
	if (stencil_next_pass.is_valid()) {
		stencil_next_pass->set_grow(p_outline_thickness);
	}
}

float BaseMaterial3D::get_stencil_effect_outline_thickness() const {
	return stencil_effect_outline_thickness;
}

RID BaseMaterial3D::get_shader_rid() const {
	const_cast<BaseMaterial3D *>(this)->_update_shader();
	return shader_rid;
}

Shader::Mode BaseMaterial3D::get_shader_mode() const {
	return Shader::MODE_SPATIAL;
}

void BaseMaterial3D::_bind_methods() {
	static_assert(sizeof(MaterialKey) == 24, "MaterialKey should be 24 bytes"); // Mariner AA3: was 16.

	ClassDB::bind_method(D_METHOD("set_albedo", "albedo"), &BaseMaterial3D::set_albedo);
	ClassDB::bind_method(D_METHOD("get_albedo"), &BaseMaterial3D::get_albedo);

	ClassDB::bind_method(D_METHOD("set_transparency", "transparency"), &BaseMaterial3D::set_transparency);
	ClassDB::bind_method(D_METHOD("get_transparency"), &BaseMaterial3D::get_transparency);

	ClassDB::bind_method(D_METHOD("set_alpha_antialiasing", "alpha_aa"), &BaseMaterial3D::set_alpha_antialiasing);
	ClassDB::bind_method(D_METHOD("get_alpha_antialiasing"), &BaseMaterial3D::get_alpha_antialiasing);

	ClassDB::bind_method(D_METHOD("set_alpha_antialiasing_edge", "edge"), &BaseMaterial3D::set_alpha_antialiasing_edge);
	ClassDB::bind_method(D_METHOD("get_alpha_antialiasing_edge"), &BaseMaterial3D::get_alpha_antialiasing_edge);

	ClassDB::bind_method(D_METHOD("set_shading_mode", "shading_mode"), &BaseMaterial3D::set_shading_mode);
	ClassDB::bind_method(D_METHOD("get_shading_mode"), &BaseMaterial3D::get_shading_mode);

	ClassDB::bind_method(D_METHOD("set_specular", "specular"), &BaseMaterial3D::set_specular);
	ClassDB::bind_method(D_METHOD("get_specular"), &BaseMaterial3D::get_specular);

	ClassDB::bind_method(D_METHOD("set_metallic", "metallic"), &BaseMaterial3D::set_metallic);
	ClassDB::bind_method(D_METHOD("get_metallic"), &BaseMaterial3D::get_metallic);

	ClassDB::bind_method(D_METHOD("set_roughness", "roughness"), &BaseMaterial3D::set_roughness);
	ClassDB::bind_method(D_METHOD("get_roughness"), &BaseMaterial3D::get_roughness);

	ClassDB::bind_method(D_METHOD("set_emission", "emission"), &BaseMaterial3D::set_emission);
	ClassDB::bind_method(D_METHOD("get_emission"), &BaseMaterial3D::get_emission);

	ClassDB::bind_method(D_METHOD("set_emission_energy_multiplier", "emission_energy_multiplier"), &BaseMaterial3D::set_emission_energy_multiplier);
	ClassDB::bind_method(D_METHOD("get_emission_energy_multiplier"), &BaseMaterial3D::get_emission_energy_multiplier);

	ClassDB::bind_method(D_METHOD("set_emission_intensity", "emission_energy_multiplier"), &BaseMaterial3D::set_emission_intensity);
	ClassDB::bind_method(D_METHOD("get_emission_intensity"), &BaseMaterial3D::get_emission_intensity);

	ClassDB::bind_method(D_METHOD("set_normal_scale", "normal_scale"), &BaseMaterial3D::set_normal_scale);
	ClassDB::bind_method(D_METHOD("get_normal_scale"), &BaseMaterial3D::get_normal_scale);

	ClassDB::bind_method(D_METHOD("set_rim", "rim"), &BaseMaterial3D::set_rim);
	ClassDB::bind_method(D_METHOD("get_rim"), &BaseMaterial3D::get_rim);

	ClassDB::bind_method(D_METHOD("set_rim_tint", "rim_tint"), &BaseMaterial3D::set_rim_tint);
	ClassDB::bind_method(D_METHOD("get_rim_tint"), &BaseMaterial3D::get_rim_tint);

	ClassDB::bind_method(D_METHOD("set_clearcoat", "clearcoat"), &BaseMaterial3D::set_clearcoat);
	ClassDB::bind_method(D_METHOD("get_clearcoat"), &BaseMaterial3D::get_clearcoat);

	ClassDB::bind_method(D_METHOD("set_clearcoat_roughness", "clearcoat_roughness"), &BaseMaterial3D::set_clearcoat_roughness);
	ClassDB::bind_method(D_METHOD("get_clearcoat_roughness"), &BaseMaterial3D::get_clearcoat_roughness);

	ClassDB::bind_method(D_METHOD("set_anisotropy", "anisotropy"), &BaseMaterial3D::set_anisotropy);
	ClassDB::bind_method(D_METHOD("get_anisotropy"), &BaseMaterial3D::get_anisotropy);

	ClassDB::bind_method(D_METHOD("set_heightmap_scale", "heightmap_scale"), &BaseMaterial3D::set_heightmap_scale);
	ClassDB::bind_method(D_METHOD("get_heightmap_scale"), &BaseMaterial3D::get_heightmap_scale);

	ClassDB::bind_method(D_METHOD("set_subsurface_scattering_strength", "strength"), &BaseMaterial3D::set_subsurface_scattering_strength);
	ClassDB::bind_method(D_METHOD("get_subsurface_scattering_strength"), &BaseMaterial3D::get_subsurface_scattering_strength);

	ClassDB::bind_method(D_METHOD("set_transmittance_color", "color"), &BaseMaterial3D::set_transmittance_color);
	ClassDB::bind_method(D_METHOD("get_transmittance_color"), &BaseMaterial3D::get_transmittance_color);

	ClassDB::bind_method(D_METHOD("set_transmittance_depth", "depth"), &BaseMaterial3D::set_transmittance_depth);
	ClassDB::bind_method(D_METHOD("get_transmittance_depth"), &BaseMaterial3D::get_transmittance_depth);

	ClassDB::bind_method(D_METHOD("set_transmittance_boost", "boost"), &BaseMaterial3D::set_transmittance_boost);
	ClassDB::bind_method(D_METHOD("get_transmittance_boost"), &BaseMaterial3D::get_transmittance_boost);

	ClassDB::bind_method(D_METHOD("set_backlight", "backlight"), &BaseMaterial3D::set_backlight);
	ClassDB::bind_method(D_METHOD("get_backlight"), &BaseMaterial3D::get_backlight);

	ClassDB::bind_method(D_METHOD("set_refraction", "refraction"), &BaseMaterial3D::set_refraction);
	ClassDB::bind_method(D_METHOD("get_refraction"), &BaseMaterial3D::get_refraction);

	ClassDB::bind_method(D_METHOD("set_point_size", "point_size"), &BaseMaterial3D::set_point_size);
	ClassDB::bind_method(D_METHOD("get_point_size"), &BaseMaterial3D::get_point_size);

	ClassDB::bind_method(D_METHOD("set_detail_uv", "detail_uv"), &BaseMaterial3D::set_detail_uv);
	ClassDB::bind_method(D_METHOD("get_detail_uv"), &BaseMaterial3D::get_detail_uv);

	ClassDB::bind_method(D_METHOD("set_blend_mode", "blend_mode"), &BaseMaterial3D::set_blend_mode);
	ClassDB::bind_method(D_METHOD("get_blend_mode"), &BaseMaterial3D::get_blend_mode);

	ClassDB::bind_method(D_METHOD("set_depth_draw_mode", "depth_draw_mode"), &BaseMaterial3D::set_depth_draw_mode);
	ClassDB::bind_method(D_METHOD("get_depth_draw_mode"), &BaseMaterial3D::get_depth_draw_mode);

	ClassDB::bind_method(D_METHOD("set_depth_test", "depth_test"), &BaseMaterial3D::set_depth_test);
	ClassDB::bind_method(D_METHOD("get_depth_test"), &BaseMaterial3D::get_depth_test);

	ClassDB::bind_method(D_METHOD("set_cull_mode", "cull_mode"), &BaseMaterial3D::set_cull_mode);
	ClassDB::bind_method(D_METHOD("get_cull_mode"), &BaseMaterial3D::get_cull_mode);

	ClassDB::bind_method(D_METHOD("set_diffuse_mode", "diffuse_mode"), &BaseMaterial3D::set_diffuse_mode);
	ClassDB::bind_method(D_METHOD("get_diffuse_mode"), &BaseMaterial3D::get_diffuse_mode);

	ClassDB::bind_method(D_METHOD("set_specular_mode", "specular_mode"), &BaseMaterial3D::set_specular_mode);
	ClassDB::bind_method(D_METHOD("get_specular_mode"), &BaseMaterial3D::get_specular_mode);

	ClassDB::bind_method(D_METHOD("set_flag", "flag", "enable"), &BaseMaterial3D::set_flag);
	ClassDB::bind_method(D_METHOD("get_flag", "flag"), &BaseMaterial3D::get_flag);

	ClassDB::bind_method(D_METHOD("set_texture_filter", "mode"), &BaseMaterial3D::set_texture_filter);
	ClassDB::bind_method(D_METHOD("get_texture_filter"), &BaseMaterial3D::get_texture_filter);

	ClassDB::bind_method(D_METHOD("set_feature", "feature", "enable"), &BaseMaterial3D::set_feature);
	ClassDB::bind_method(D_METHOD("get_feature", "feature"), &BaseMaterial3D::get_feature);

	ClassDB::bind_method(D_METHOD("set_texture", "param", "texture"), &BaseMaterial3D::set_texture);
	ClassDB::bind_method(D_METHOD("get_texture", "param"), &BaseMaterial3D::get_texture);

	ClassDB::bind_method(D_METHOD("set_detail_blend_mode", "detail_blend_mode"), &BaseMaterial3D::set_detail_blend_mode);
	ClassDB::bind_method(D_METHOD("get_detail_blend_mode"), &BaseMaterial3D::get_detail_blend_mode);

	ClassDB::bind_method(D_METHOD("set_uv1_scale", "scale"), &BaseMaterial3D::set_uv1_scale);
	ClassDB::bind_method(D_METHOD("get_uv1_scale"), &BaseMaterial3D::get_uv1_scale);

	ClassDB::bind_method(D_METHOD("set_uv1_offset", "offset"), &BaseMaterial3D::set_uv1_offset);
	ClassDB::bind_method(D_METHOD("get_uv1_offset"), &BaseMaterial3D::get_uv1_offset);

	ClassDB::bind_method(D_METHOD("set_uv1_triplanar_blend_sharpness", "sharpness"), &BaseMaterial3D::set_uv1_triplanar_blend_sharpness);
	ClassDB::bind_method(D_METHOD("get_uv1_triplanar_blend_sharpness"), &BaseMaterial3D::get_uv1_triplanar_blend_sharpness);

	ClassDB::bind_method(D_METHOD("set_uv2_scale", "scale"), &BaseMaterial3D::set_uv2_scale);
	ClassDB::bind_method(D_METHOD("get_uv2_scale"), &BaseMaterial3D::get_uv2_scale);

	ClassDB::bind_method(D_METHOD("set_uv2_offset", "offset"), &BaseMaterial3D::set_uv2_offset);
	ClassDB::bind_method(D_METHOD("get_uv2_offset"), &BaseMaterial3D::get_uv2_offset);

	ClassDB::bind_method(D_METHOD("set_uv2_triplanar_blend_sharpness", "sharpness"), &BaseMaterial3D::set_uv2_triplanar_blend_sharpness);
	ClassDB::bind_method(D_METHOD("get_uv2_triplanar_blend_sharpness"), &BaseMaterial3D::get_uv2_triplanar_blend_sharpness);

	ClassDB::bind_method(D_METHOD("set_billboard_mode", "mode"), &BaseMaterial3D::set_billboard_mode);
	ClassDB::bind_method(D_METHOD("get_billboard_mode"), &BaseMaterial3D::get_billboard_mode);

	ClassDB::bind_method(D_METHOD("set_particles_anim_h_frames", "frames"), &BaseMaterial3D::set_particles_anim_h_frames);
	ClassDB::bind_method(D_METHOD("get_particles_anim_h_frames"), &BaseMaterial3D::get_particles_anim_h_frames);

	ClassDB::bind_method(D_METHOD("set_particles_anim_v_frames", "frames"), &BaseMaterial3D::set_particles_anim_v_frames);
	ClassDB::bind_method(D_METHOD("get_particles_anim_v_frames"), &BaseMaterial3D::get_particles_anim_v_frames);

	ClassDB::bind_method(D_METHOD("set_particles_anim_loop", "loop"), &BaseMaterial3D::set_particles_anim_loop);
	ClassDB::bind_method(D_METHOD("get_particles_anim_loop"), &BaseMaterial3D::get_particles_anim_loop);

	ClassDB::bind_method(D_METHOD("set_heightmap_self_shadow", "enable"), &BaseMaterial3D::set_heightmap_self_shadow);
	ClassDB::bind_method(D_METHOD("is_heightmap_self_shadow_enabled"), &BaseMaterial3D::is_heightmap_self_shadow_enabled);
	ClassDB::bind_method(D_METHOD("set_heightmap_self_shadow_strength", "strength"), &BaseMaterial3D::set_heightmap_self_shadow_strength);
	ClassDB::bind_method(D_METHOD("get_heightmap_self_shadow_strength"), &BaseMaterial3D::get_heightmap_self_shadow_strength);
	ClassDB::bind_static_method("BaseMaterial3D", D_METHOD("get_metal_albedo", "metal"), &BaseMaterial3D::get_metal_albedo); // Mariner M8.
	// Mariner M6, F5, F6, M4.
	ClassDB::bind_method(D_METHOD("set_wetness_amount", "value"), &BaseMaterial3D::set_wetness_amount); // Mariner G4.
	ClassDB::bind_method(D_METHOD("get_wetness_amount"), &BaseMaterial3D::get_wetness_amount);
	ClassDB::bind_method(D_METHOD("set_wetness_porosity", "value"), &BaseMaterial3D::set_wetness_porosity);
	ClassDB::bind_method(D_METHOD("get_wetness_porosity"), &BaseMaterial3D::get_wetness_porosity);
	ClassDB::bind_method(D_METHOD("set_wetness_puddles", "value"), &BaseMaterial3D::set_wetness_puddles);
	ClassDB::bind_method(D_METHOD("get_wetness_puddles"), &BaseMaterial3D::get_wetness_puddles);
	ClassDB::bind_method(D_METHOD("set_snow_amount", "value"), &BaseMaterial3D::set_snow_amount); // Mariner G4b.
	ClassDB::bind_method(D_METHOD("get_snow_amount"), &BaseMaterial3D::get_snow_amount);
	ClassDB::bind_method(D_METHOD("set_snow_color", "color"), &BaseMaterial3D::set_snow_color);
	ClassDB::bind_method(D_METHOD("get_snow_color"), &BaseMaterial3D::get_snow_color);
	ClassDB::bind_method(D_METHOD("set_snow_roughness", "value"), &BaseMaterial3D::set_snow_roughness);
	ClassDB::bind_method(D_METHOD("get_snow_roughness"), &BaseMaterial3D::get_snow_roughness);
	ClassDB::bind_method(D_METHOD("set_snow_max_slope", "degrees"), &BaseMaterial3D::set_snow_max_slope);
	ClassDB::bind_method(D_METHOD("get_snow_max_slope"), &BaseMaterial3D::get_snow_max_slope);
	ClassDB::bind_method(D_METHOD("set_snow_relief", "value"), &BaseMaterial3D::set_snow_relief);
	ClassDB::bind_method(D_METHOD("get_snow_relief"), &BaseMaterial3D::get_snow_relief);
	ClassDB::bind_method(D_METHOD("set_interior_rooms", "value"), &BaseMaterial3D::set_interior_rooms); // Mariner G5a.
	ClassDB::bind_method(D_METHOD("get_interior_rooms"), &BaseMaterial3D::get_interior_rooms);
	ClassDB::bind_method(D_METHOD("set_interior_room_size", "value"), &BaseMaterial3D::set_interior_room_size);
	ClassDB::bind_method(D_METHOD("get_interior_room_size"), &BaseMaterial3D::get_interior_room_size);
	ClassDB::bind_method(D_METHOD("set_interior_atlas_size", "value"), &BaseMaterial3D::set_interior_atlas_size);
	ClassDB::bind_method(D_METHOD("get_interior_atlas_size"), &BaseMaterial3D::get_interior_atlas_size);
	ClassDB::bind_method(D_METHOD("set_interior_back_wall", "value"), &BaseMaterial3D::set_interior_back_wall);
	ClassDB::bind_method(D_METHOD("get_interior_back_wall"), &BaseMaterial3D::get_interior_back_wall);
	ClassDB::bind_method(D_METHOD("set_interior_brightness", "value"), &BaseMaterial3D::set_interior_brightness);
	ClassDB::bind_method(D_METHOD("get_interior_brightness"), &BaseMaterial3D::get_interior_brightness);
	ClassDB::bind_method(D_METHOD("set_interior_lights", "value"), &BaseMaterial3D::set_interior_lights);
	ClassDB::bind_method(D_METHOD("get_interior_lights"), &BaseMaterial3D::get_interior_lights);
	ClassDB::bind_method(D_METHOD("set_interior_lit_fraction", "value"), &BaseMaterial3D::set_interior_lit_fraction);
	ClassDB::bind_method(D_METHOD("get_interior_lit_fraction"), &BaseMaterial3D::get_interior_lit_fraction);
	ClassDB::bind_method(D_METHOD("set_interior_light_color", "value"), &BaseMaterial3D::set_interior_light_color);
	ClassDB::bind_method(D_METHOD("get_interior_light_color"), &BaseMaterial3D::get_interior_light_color);
	ClassDB::bind_method(D_METHOD("set_interior_curtains", "value"), &BaseMaterial3D::set_interior_curtains); // Mariner G5d.
	ClassDB::bind_method(D_METHOD("get_interior_curtains"), &BaseMaterial3D::get_interior_curtains);
	ClassDB::bind_method(D_METHOD("set_interior_blinds", "value"), &BaseMaterial3D::set_interior_blinds);
	ClassDB::bind_method(D_METHOD("get_interior_blinds"), &BaseMaterial3D::get_interior_blinds);
	ClassDB::bind_method(D_METHOD("set_interior_curtain_color", "value"), &BaseMaterial3D::set_interior_curtain_color);
	ClassDB::bind_method(D_METHOD("get_interior_curtain_color"), &BaseMaterial3D::get_interior_curtain_color);
	ClassDB::bind_method(D_METHOD("set_sheen_color", "value"), &BaseMaterial3D::set_sheen_color);
	ClassDB::bind_method(D_METHOD("get_sheen_color"), &BaseMaterial3D::get_sheen_color);
	ClassDB::bind_method(D_METHOD("set_sheen_roughness", "value"), &BaseMaterial3D::set_sheen_roughness);
	ClassDB::bind_method(D_METHOD("get_sheen_roughness"), &BaseMaterial3D::get_sheen_roughness);
	ClassDB::bind_method(D_METHOD("set_iridescence", "value"), &BaseMaterial3D::set_iridescence);
	ClassDB::bind_method(D_METHOD("get_iridescence"), &BaseMaterial3D::get_iridescence);
	ClassDB::bind_method(D_METHOD("set_iridescence_ior", "value"), &BaseMaterial3D::set_iridescence_ior);
	ClassDB::bind_method(D_METHOD("get_iridescence_ior"), &BaseMaterial3D::get_iridescence_ior);
	ClassDB::bind_method(D_METHOD("set_iridescence_thickness_min", "value"), &BaseMaterial3D::set_iridescence_thickness_min);
	ClassDB::bind_method(D_METHOD("get_iridescence_thickness_min"), &BaseMaterial3D::get_iridescence_thickness_min);
	ClassDB::bind_method(D_METHOD("set_iridescence_thickness_max", "value"), &BaseMaterial3D::set_iridescence_thickness_max);
	ClassDB::bind_method(D_METHOD("get_iridescence_thickness_max"), &BaseMaterial3D::get_iridescence_thickness_max);
	ClassDB::bind_method(D_METHOD("set_refraction_ior", "value"), &BaseMaterial3D::set_refraction_ior);
	ClassDB::bind_method(D_METHOD("get_refraction_ior"), &BaseMaterial3D::get_refraction_ior);
	ClassDB::bind_method(D_METHOD("set_refraction_thickness", "value"), &BaseMaterial3D::set_refraction_thickness);
	ClassDB::bind_method(D_METHOD("get_refraction_thickness"), &BaseMaterial3D::get_refraction_thickness);
	ClassDB::bind_method(D_METHOD("set_refraction_absorption_color", "value"), &BaseMaterial3D::set_refraction_absorption_color);
	ClassDB::bind_method(D_METHOD("get_refraction_absorption_color"), &BaseMaterial3D::get_refraction_absorption_color);
	ClassDB::bind_method(D_METHOD("set_refraction_absorption_distance", "value"), &BaseMaterial3D::set_refraction_absorption_distance);
	ClassDB::bind_method(D_METHOD("get_refraction_absorption_distance"), &BaseMaterial3D::get_refraction_absorption_distance);
	ClassDB::bind_method(D_METHOD("set_refraction_physical", "enable"), &BaseMaterial3D::set_refraction_physical);
	ClassDB::bind_method(D_METHOD("is_refraction_physical"), &BaseMaterial3D::is_refraction_physical);
	ClassDB::bind_method(D_METHOD("set_detail_normal_rnm", "enable"), &BaseMaterial3D::set_detail_normal_rnm);
	ClassDB::bind_method(D_METHOD("is_detail_normal_rnm"), &BaseMaterial3D::is_detail_normal_rnm);
	ClassDB::bind_method(D_METHOD("set_heightmap_depth_offset", "enable"), &BaseMaterial3D::set_heightmap_depth_offset);
	ClassDB::bind_method(D_METHOD("is_heightmap_depth_offset_enabled"), &BaseMaterial3D::is_heightmap_depth_offset_enabled);
	ClassDB::bind_method(D_METHOD("set_heightmap_impostor", "enable"), &BaseMaterial3D::set_heightmap_impostor); // Mariner G5b.
	ClassDB::bind_method(D_METHOD("is_heightmap_impostor_enabled"), &BaseMaterial3D::is_heightmap_impostor_enabled);
	ClassDB::bind_method(D_METHOD("set_heightmap_impostor_faces", "faces"), &BaseMaterial3D::set_heightmap_impostor_faces); // Mariner G5c.
	ClassDB::bind_method(D_METHOD("get_heightmap_impostor_faces"), &BaseMaterial3D::get_heightmap_impostor_faces);
	ClassDB::bind_method(D_METHOD("set_planar_reflection_amount", "value"), &BaseMaterial3D::set_planar_reflection_amount); // Mariner G6.
	ClassDB::bind_method(D_METHOD("get_planar_reflection_amount"), &BaseMaterial3D::get_planar_reflection_amount);
	ClassDB::bind_method(D_METHOD("set_planar_reflection_distortion", "value"), &BaseMaterial3D::set_planar_reflection_distortion);
	ClassDB::bind_method(D_METHOD("get_planar_reflection_distortion"), &BaseMaterial3D::get_planar_reflection_distortion);
	ClassDB::bind_method(D_METHOD("set_displacement_scale", "value"), &BaseMaterial3D::set_displacement_scale); // Mariner G7.
	ClassDB::bind_method(D_METHOD("get_displacement_scale"), &BaseMaterial3D::get_displacement_scale);
	ClassDB::bind_method(D_METHOD("set_displacement_midlevel", "value"), &BaseMaterial3D::set_displacement_midlevel);
	ClassDB::bind_method(D_METHOD("get_displacement_midlevel"), &BaseMaterial3D::get_displacement_midlevel);
	ClassDB::bind_method(D_METHOD("set_alpha_mask_mode", "mode"), &BaseMaterial3D::set_alpha_mask_mode);
	ClassDB::bind_method(D_METHOD("get_alpha_mask_mode"), &BaseMaterial3D::get_alpha_mask_mode);
	ClassDB::bind_static_method("BaseMaterial3D", D_METHOD("bake_alpha_mask", "source", "size", "mode", "range"), &BaseMaterial3D::bake_alpha_mask, DEFVAL(4.0));

	ClassDB::bind_method(D_METHOD("set_displacement_texture_lod", "value"), &BaseMaterial3D::set_displacement_texture_lod);
	ClassDB::bind_method(D_METHOD("get_displacement_texture_lod"), &BaseMaterial3D::get_displacement_texture_lod);
	ClassDB::bind_method(D_METHOD("set_displacement_normal", "enable"), &BaseMaterial3D::set_displacement_normal);
	ClassDB::bind_method(D_METHOD("is_displacement_normal_enabled"), &BaseMaterial3D::is_displacement_normal_enabled);
	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax", "enable"), &BaseMaterial3D::set_heightmap_deep_parallax);
	ClassDB::bind_method(D_METHOD("is_heightmap_deep_parallax_enabled"), &BaseMaterial3D::is_heightmap_deep_parallax_enabled);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_min_layers", "layer"), &BaseMaterial3D::set_heightmap_deep_parallax_min_layers);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_min_layers"), &BaseMaterial3D::get_heightmap_deep_parallax_min_layers);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_max_layers", "layer"), &BaseMaterial3D::set_heightmap_deep_parallax_max_layers);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_max_layers"), &BaseMaterial3D::get_heightmap_deep_parallax_max_layers);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_flip_tangent", "flip"), &BaseMaterial3D::set_heightmap_deep_parallax_flip_tangent);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_flip_tangent"), &BaseMaterial3D::get_heightmap_deep_parallax_flip_tangent);

	ClassDB::bind_method(D_METHOD("set_heightmap_deep_parallax_flip_binormal", "flip"), &BaseMaterial3D::set_heightmap_deep_parallax_flip_binormal);
	ClassDB::bind_method(D_METHOD("get_heightmap_deep_parallax_flip_binormal"), &BaseMaterial3D::get_heightmap_deep_parallax_flip_binormal);

	ClassDB::bind_method(D_METHOD("set_grow", "amount"), &BaseMaterial3D::set_grow);
	ClassDB::bind_method(D_METHOD("get_grow"), &BaseMaterial3D::get_grow);

	ClassDB::bind_method(D_METHOD("set_emission_operator", "operator"), &BaseMaterial3D::set_emission_operator);
	ClassDB::bind_method(D_METHOD("get_emission_operator"), &BaseMaterial3D::get_emission_operator);

	ClassDB::bind_method(D_METHOD("set_ao_light_affect", "amount"), &BaseMaterial3D::set_ao_light_affect);
	ClassDB::bind_method(D_METHOD("get_ao_light_affect"), &BaseMaterial3D::get_ao_light_affect);

	ClassDB::bind_method(D_METHOD("set_alpha_scissor_threshold", "threshold"), &BaseMaterial3D::set_alpha_scissor_threshold);
	ClassDB::bind_method(D_METHOD("get_alpha_scissor_threshold"), &BaseMaterial3D::get_alpha_scissor_threshold);

	ClassDB::bind_method(D_METHOD("set_alpha_hash_scale", "threshold"), &BaseMaterial3D::set_alpha_hash_scale);
	ClassDB::bind_method(D_METHOD("get_alpha_hash_scale"), &BaseMaterial3D::get_alpha_hash_scale);

	ClassDB::bind_method(D_METHOD("set_wire_antialiasing", "enable"), &BaseMaterial3D::set_wire_antialiasing);
	ClassDB::bind_method(D_METHOD("is_wire_antialiasing"), &BaseMaterial3D::is_wire_antialiasing);
	ClassDB::bind_method(D_METHOD("set_grow_enabled", "enable"), &BaseMaterial3D::set_grow_enabled);
	ClassDB::bind_method(D_METHOD("is_grow_enabled"), &BaseMaterial3D::is_grow_enabled);

	ClassDB::bind_method(D_METHOD("set_metallic_texture_channel", "channel"), &BaseMaterial3D::set_metallic_texture_channel);
	ClassDB::bind_method(D_METHOD("get_metallic_texture_channel"), &BaseMaterial3D::get_metallic_texture_channel);

	ClassDB::bind_method(D_METHOD("set_roughness_texture_channel", "channel"), &BaseMaterial3D::set_roughness_texture_channel);
	ClassDB::bind_method(D_METHOD("get_roughness_texture_channel"), &BaseMaterial3D::get_roughness_texture_channel);

	ClassDB::bind_method(D_METHOD("set_ao_texture_channel", "channel"), &BaseMaterial3D::set_ao_texture_channel);
	ClassDB::bind_method(D_METHOD("get_ao_texture_channel"), &BaseMaterial3D::get_ao_texture_channel);

	ClassDB::bind_method(D_METHOD("set_refraction_texture_channel", "channel"), &BaseMaterial3D::set_refraction_texture_channel);
	ClassDB::bind_method(D_METHOD("get_refraction_texture_channel"), &BaseMaterial3D::get_refraction_texture_channel);

	ClassDB::bind_method(D_METHOD("set_proximity_fade_enabled", "enabled"), &BaseMaterial3D::set_proximity_fade_enabled);
	ClassDB::bind_method(D_METHOD("is_proximity_fade_enabled"), &BaseMaterial3D::is_proximity_fade_enabled);

	ClassDB::bind_method(D_METHOD("set_proximity_fade_distance", "distance"), &BaseMaterial3D::set_proximity_fade_distance);
	ClassDB::bind_method(D_METHOD("get_proximity_fade_distance"), &BaseMaterial3D::get_proximity_fade_distance);

	ClassDB::bind_method(D_METHOD("set_msdf_pixel_range", "range"), &BaseMaterial3D::set_msdf_pixel_range);
	ClassDB::bind_method(D_METHOD("get_msdf_pixel_range"), &BaseMaterial3D::get_msdf_pixel_range);

	ClassDB::bind_method(D_METHOD("set_msdf_outline_size", "size"), &BaseMaterial3D::set_msdf_outline_size);
	ClassDB::bind_method(D_METHOD("get_msdf_outline_size"), &BaseMaterial3D::get_msdf_outline_size);

	ClassDB::bind_method(D_METHOD("set_distance_fade", "mode"), &BaseMaterial3D::set_distance_fade);
	ClassDB::bind_method(D_METHOD("get_distance_fade"), &BaseMaterial3D::get_distance_fade);

	ClassDB::bind_method(D_METHOD("set_distance_fade_max_distance", "distance"), &BaseMaterial3D::set_distance_fade_max_distance);
	ClassDB::bind_method(D_METHOD("get_distance_fade_max_distance"), &BaseMaterial3D::get_distance_fade_max_distance);

	ClassDB::bind_method(D_METHOD("set_distance_fade_min_distance", "distance"), &BaseMaterial3D::set_distance_fade_min_distance);
	ClassDB::bind_method(D_METHOD("get_distance_fade_min_distance"), &BaseMaterial3D::get_distance_fade_min_distance);

	ClassDB::bind_method(D_METHOD("set_z_clip_scale", "scale"), &BaseMaterial3D::set_z_clip_scale);
	ClassDB::bind_method(D_METHOD("get_z_clip_scale"), &BaseMaterial3D::get_z_clip_scale);

	ClassDB::bind_method(D_METHOD("set_fov_override", "scale"), &BaseMaterial3D::set_fov_override);
	ClassDB::bind_method(D_METHOD("get_fov_override"), &BaseMaterial3D::get_fov_override);

	ClassDB::bind_method(D_METHOD("set_stencil_mode", "stencil_mode"), &BaseMaterial3D::set_stencil_mode);
	ClassDB::bind_method(D_METHOD("get_stencil_mode"), &BaseMaterial3D::get_stencil_mode);

	ClassDB::bind_method(D_METHOD("set_stencil_flags", "stencil_flags"), &BaseMaterial3D::set_stencil_flags);
	ClassDB::bind_method(D_METHOD("get_stencil_flags"), &BaseMaterial3D::get_stencil_flags);

	ClassDB::bind_method(D_METHOD("set_stencil_compare", "stencil_compare"), &BaseMaterial3D::set_stencil_compare);
	ClassDB::bind_method(D_METHOD("get_stencil_compare"), &BaseMaterial3D::get_stencil_compare);

	ClassDB::bind_method(D_METHOD("set_stencil_reference", "stencil_reference"), &BaseMaterial3D::set_stencil_reference);
	ClassDB::bind_method(D_METHOD("get_stencil_reference"), &BaseMaterial3D::get_stencil_reference);

	ClassDB::bind_method(D_METHOD("set_stencil_effect_color", "stencil_color"), &BaseMaterial3D::set_stencil_effect_color);
	ClassDB::bind_method(D_METHOD("get_stencil_effect_color"), &BaseMaterial3D::get_stencil_effect_color);

	ClassDB::bind_method(D_METHOD("set_stencil_effect_outline_thickness", "stencil_outline_thickness"), &BaseMaterial3D::set_stencil_effect_outline_thickness);
	ClassDB::bind_method(D_METHOD("get_stencil_effect_outline_thickness"), &BaseMaterial3D::get_stencil_effect_outline_thickness);

	ADD_GROUP("Transparency", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "transparency", PROPERTY_HINT_ENUM, "Disabled,Alpha,Alpha Scissor,Alpha Hash,Depth Pre-Pass"), "set_transparency", "get_transparency");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_scissor_threshold", PROPERTY_HINT_RANGE, "0,1,0.001"), "set_alpha_scissor_threshold", "get_alpha_scissor_threshold");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_hash_scale", PROPERTY_HINT_RANGE, "0,2,0.01"), "set_alpha_hash_scale", "get_alpha_hash_scale");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "alpha_antialiasing_mode", PROPERTY_HINT_ENUM, "Disabled,Alpha Edge Blend,Alpha Edge Clip,Fringe"), "set_alpha_antialiasing", "get_alpha_antialiasing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "alpha_mask_mode", PROPERTY_HINT_ENUM, "Albedo Alpha,SDF,MSDF"), "set_alpha_mask_mode", "get_alpha_mask_mode");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "alpha_mask_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ALPHA_MASK);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "alpha_antialiasing_edge", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_alpha_antialiasing_edge", "get_alpha_antialiasing_edge");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "wire_antialiasing"), "set_wire_antialiasing", "is_wire_antialiasing");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "blend_mode", PROPERTY_HINT_ENUM, "Mix,Add,Subtract,Multiply,Premultiplied Alpha"), "set_blend_mode", "get_blend_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "cull_mode", PROPERTY_HINT_ENUM, "Back,Front,Disabled"), "set_cull_mode", "get_cull_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "depth_draw_mode", PROPERTY_HINT_ENUM, "Opaque Only,Always,Never"), "set_depth_draw_mode", "get_depth_draw_mode");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "no_depth_test"), "set_flag", "get_flag", FLAG_DISABLE_DEPTH_TEST);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "depth_test", PROPERTY_HINT_ENUM, "Default,Inverted"), "set_depth_test", "get_depth_test");

	ADD_GROUP("Shading", "");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "shading_mode", PROPERTY_HINT_ENUM, "Unshaded,Per-Pixel,Per-Vertex"), "set_shading_mode", "get_shading_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "diffuse_mode", PROPERTY_HINT_ENUM, "Burley,Lambert,Lambert Wrap,Toon"), "set_diffuse_mode", "get_diffuse_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "specular_mode", PROPERTY_HINT_ENUM, "SchlickGGX,Toon,Disabled"), "set_specular_mode", "get_specular_mode");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_ambient_light"), "set_flag", "get_flag", FLAG_DISABLE_AMBIENT_LIGHT);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_fog"), "set_flag", "get_flag", FLAG_DISABLE_FOG);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_specular_occlusion"), "set_flag", "get_flag", FLAG_DISABLE_SPECULAR_OCCLUSION);

	ADD_GROUP("Vertex Color", "vertex_color");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "vertex_color_use_as_albedo"), "set_flag", "get_flag", FLAG_ALBEDO_FROM_VERTEX_COLOR);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "vertex_color_is_srgb"), "set_flag", "get_flag", FLAG_SRGB_VERTEX_COLOR);

	ADD_GROUP("Albedo", "albedo_");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "albedo_color"), "set_albedo", "get_albedo");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "albedo_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ALBEDO);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "albedo_texture_force_srgb"), "set_flag", "get_flag", FLAG_ALBEDO_TEXTURE_FORCE_SRGB);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "albedo_texture_msdf"), "set_flag", "get_flag", FLAG_ALBEDO_TEXTURE_MSDF);

	ADD_GROUP("ORM", "orm_");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "orm_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ORM);

	ADD_GROUP("Metallic", "metallic_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_metallic", "get_metallic");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "metallic_specular", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_specular", "get_specular");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "metallic_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_METALLIC);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "metallic_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_metallic_texture_channel", "get_metallic_texture_channel");

	ADD_GROUP("Roughness", "roughness_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_roughness", "get_roughness");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "roughness_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_ROUGHNESS);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "roughness_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_roughness_texture_channel", "get_roughness_texture_channel");

	ADD_GROUP("Emission", "emission_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "emission_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_EMISSION);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "emission", PROPERTY_HINT_COLOR_NO_ALPHA), "set_emission", "get_emission");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_energy_multiplier", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_emission_energy_multiplier", "get_emission_energy_multiplier");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "emission_intensity", PROPERTY_HINT_RANGE, "0,100000.0,0.01,or_greater,suffix:nt"), "set_emission_intensity", "get_emission_intensity");

	ADD_PROPERTY(PropertyInfo(Variant::INT, "emission_operator", PROPERTY_HINT_ENUM, "Add,Multiply"), "set_emission_operator", "get_emission_operator");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "emission_on_uv2"), "set_flag", "get_flag", FLAG_EMISSION_ON_UV2);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "emission_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_EMISSION);

	ADD_GROUP("Normal Map", "normal_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "normal_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_NORMAL_MAPPING);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "normal_scale", PROPERTY_HINT_RANGE, "-16,16,0.01"), "set_normal_scale", "get_normal_scale");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "normal_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_NORMAL);

	ADD_GROUP("Bent Normal Map", "bent_normal_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "bent_normal_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_BENT_NORMAL_MAPPING);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "bent_normal_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_BENT_NORMAL);

	ADD_GROUP("Rim", "rim_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "rim_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_RIM);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rim", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rim", "get_rim");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "rim_tint", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_rim_tint", "get_rim_tint");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "rim_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_RIM);

	ADD_GROUP("Clearcoat", "clearcoat_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "clearcoat_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_CLEARCOAT);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clearcoat", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_clearcoat", "get_clearcoat");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "clearcoat_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_clearcoat_roughness", "get_clearcoat_roughness");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "clearcoat_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_CLEARCOAT);

	// Mariner M6.
	// Mariner G4.
	ADD_GROUP("Wetness", "wetness_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "wetness_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_WETNESS);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness_amount", "get_wetness_amount");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness_porosity", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness_porosity", "get_wetness_porosity");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wetness_puddles", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_wetness_puddles", "get_wetness_puddles");

	ADD_GROUP("Snow", "snow_"); // Mariner G4b.
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "snow_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_SNOW);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_snow_amount", "get_snow_amount");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "snow_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_snow_color", "get_snow_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_snow_roughness", "get_snow_roughness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_max_slope", PROPERTY_HINT_RANGE, "0,90,0.1,degrees"), "set_snow_max_slope", "get_snow_max_slope");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "snow_relief", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_snow_relief", "get_snow_relief");

	// Mariner G5a.
	ADD_GROUP("Interior", "interior_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "interior_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_INTERIOR);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "interior_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_INTERIOR);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2I, "interior_atlas_size", PROPERTY_HINT_RANGE, "1,64,1"), "set_interior_atlas_size", "get_interior_atlas_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interior_back_wall", PROPERTY_HINT_RANGE, "0.05,0.95,0.01"), "set_interior_back_wall", "get_interior_back_wall");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "interior_mask_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_INTERIOR_MASK);
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR2, "interior_rooms", PROPERTY_HINT_RANGE, "0.001,1024,0.001,or_greater"), "set_interior_rooms", "get_interior_rooms");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "interior_room_size", PROPERTY_HINT_RANGE, "0.01,100,0.01,or_greater,suffix:m"), "set_interior_room_size", "get_interior_room_size");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interior_brightness", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_interior_brightness", "get_interior_brightness");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interior_lights", PROPERTY_HINT_RANGE, "0,16,0.01,or_greater"), "set_interior_lights", "get_interior_lights");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interior_lit_fraction", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_interior_lit_fraction", "get_interior_lit_fraction");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "interior_light_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_interior_light_color", "get_interior_light_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interior_curtains", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_interior_curtains", "get_interior_curtains"); // Mariner G5d.
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "interior_blinds", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_interior_blinds", "get_interior_blinds");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "interior_curtain_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_interior_curtain_color", "get_interior_curtain_color");

	// Mariner G6.
	ADD_GROUP("Planar Reflection", "planar_reflection_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "planar_reflection_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_PLANAR_REFLECTION);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "planar_reflection_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_PLANAR_REFLECTION);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planar_reflection_amount", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_planar_reflection_amount", "get_planar_reflection_amount");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "planar_reflection_distortion", PROPERTY_HINT_RANGE, "0,0.2,0.001"), "set_planar_reflection_distortion", "get_planar_reflection_distortion");

	ADD_GROUP("Displacement", "displacement_"); // Mariner G7.
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "displacement_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_DISPLACEMENT);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "displacement_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_DISPLACEMENT);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "displacement_scale", PROPERTY_HINT_RANGE, "-1,1,0.001,or_less,or_greater,suffix:m"), "set_displacement_scale", "get_displacement_scale");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "displacement_midlevel", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_displacement_midlevel", "get_displacement_midlevel");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "displacement_texture_lod", PROPERTY_HINT_RANGE, "0,12,0.1"), "set_displacement_texture_lod", "get_displacement_texture_lod");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "displacement_normal"), "set_displacement_normal", "is_displacement_normal_enabled");

	ADD_GROUP("Sheen", "sheen_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "sheen_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_SHEEN);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "sheen_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_sheen_color", "get_sheen_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "sheen_roughness", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_sheen_roughness", "get_sheen_roughness");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "sheen_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_SHEEN);

	// Mariner F5.
	ADD_GROUP("Iridescence", "iridescence_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "iridescence_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_IRIDESCENCE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "iridescence", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_iridescence", "get_iridescence");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "iridescence_ior", PROPERTY_HINT_RANGE, "1,2.333,0.01"), "set_iridescence_ior", "get_iridescence_ior");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "iridescence_thickness_min", PROPERTY_HINT_RANGE, "0,1200,1,suffix:nm"), "set_iridescence_thickness_min", "get_iridescence_thickness_min");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "iridescence_thickness_max", PROPERTY_HINT_RANGE, "0,1200,1,suffix:nm"), "set_iridescence_thickness_max", "get_iridescence_thickness_max");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "iridescence_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_IRIDESCENCE);

	ADD_GROUP("Anisotropy", "anisotropy_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "anisotropy_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_ANISOTROPY);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "anisotropy", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_anisotropy", "get_anisotropy");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "anisotropy_flowmap", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_FLOWMAP);

	ADD_GROUP("Ambient Occlusion", "ao_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "ao_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_AMBIENT_OCCLUSION);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "ao_light_affect", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_ao_light_affect", "get_ao_light_affect");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "ao_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_AMBIENT_OCCLUSION);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "ao_on_uv2"), "set_flag", "get_flag", FLAG_AO_ON_UV2);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "ao_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_ao_texture_channel", "get_ao_texture_channel");

	ADD_GROUP("Height", "heightmap_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "heightmap_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_HEIGHT_MAPPING);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heightmap_scale", PROPERTY_HINT_RANGE, "-16,16,0.001,or_less,or_greater"), "set_heightmap_scale", "get_heightmap_scale");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_deep_parallax"), "set_heightmap_deep_parallax", "is_heightmap_deep_parallax_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "heightmap_min_layers", PROPERTY_HINT_RANGE, "1,64,1"), "set_heightmap_deep_parallax_min_layers", "get_heightmap_deep_parallax_min_layers");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "heightmap_max_layers", PROPERTY_HINT_RANGE, "1,64,1"), "set_heightmap_deep_parallax_max_layers", "get_heightmap_deep_parallax_max_layers");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_flip_tangent"), "set_heightmap_deep_parallax_flip_tangent", "get_heightmap_deep_parallax_flip_tangent");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_flip_binormal"), "set_heightmap_deep_parallax_flip_binormal", "get_heightmap_deep_parallax_flip_binormal");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_self_shadow"), "set_heightmap_self_shadow", "is_heightmap_self_shadow_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "heightmap_self_shadow_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_heightmap_self_shadow_strength", "get_heightmap_self_shadow_strength");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_depth_offset"), "set_heightmap_depth_offset", "is_heightmap_depth_offset_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "heightmap_impostor"), "set_heightmap_impostor", "is_heightmap_impostor_enabled"); // Mariner G5b.
	ADD_PROPERTY(PropertyInfo(Variant::PACKED_VECTOR4_ARRAY, "heightmap_impostor_faces", PROPERTY_HINT_NONE, "", PROPERTY_USAGE_STORAGE | PROPERTY_USAGE_EDITOR | PROPERTY_USAGE_READ_ONLY), "set_heightmap_impostor_faces", "get_heightmap_impostor_faces"); // Mariner G5c.
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "heightmap_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_HEIGHTMAP);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "heightmap_flip_texture"), "set_flag", "get_flag", FLAG_INVERT_HEIGHTMAP);

	ADD_GROUP("Subsurf Scatter", "subsurf_scatter_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "subsurf_scatter_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_SUBSURFACE_SCATTERING);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "subsurf_scatter_strength", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_subsurface_scattering_strength", "get_subsurface_scattering_strength");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "subsurf_scatter_skin_mode"), "set_flag", "get_flag", FLAG_SUBSURFACE_MODE_SKIN);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "subsurf_scatter_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_SUBSURFACE_SCATTERING);

	ADD_SUBGROUP("Transmittance", "subsurf_scatter_transmittance_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "subsurf_scatter_transmittance_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_SUBSURFACE_TRANSMITTANCE);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "subsurf_scatter_transmittance_color"), "set_transmittance_color", "get_transmittance_color");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "subsurf_scatter_transmittance_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_SUBSURFACE_TRANSMITTANCE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "subsurf_scatter_transmittance_depth", PROPERTY_HINT_RANGE, "0.001,8,0.001,or_greater"), "set_transmittance_depth", "get_transmittance_depth");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "subsurf_scatter_transmittance_boost", PROPERTY_HINT_RANGE, "0.00,1.0,0.01"), "set_transmittance_boost", "get_transmittance_boost");

	ADD_GROUP("Back Lighting", "backlight_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "backlight_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_BACKLIGHT);
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "backlight", PROPERTY_HINT_COLOR_NO_ALPHA), "set_backlight", "get_backlight");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "backlight_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_BACKLIGHT);

	ADD_GROUP("Refraction", "refraction_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "refraction_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_REFRACTION);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "refraction_scale", PROPERTY_HINT_RANGE, "-1,1,0.01"), "set_refraction", "get_refraction");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "refraction_texture", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_REFRACTION);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "refraction_texture_channel", PROPERTY_HINT_ENUM, "Red,Green,Blue,Alpha,Gray"), "set_refraction_texture_channel", "get_refraction_texture_channel");
	// Mariner F6.
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "refraction_physical"), "set_refraction_physical", "is_refraction_physical");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "refraction_ior", PROPERTY_HINT_RANGE, "1,3,0.01"), "set_refraction_ior", "get_refraction_ior");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "refraction_thickness", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m"), "set_refraction_thickness", "get_refraction_thickness");
	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "refraction_absorption_color", PROPERTY_HINT_COLOR_NO_ALPHA), "set_refraction_absorption_color", "get_refraction_absorption_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "refraction_absorption_distance", PROPERTY_HINT_RANGE, "0.001,10,0.001,or_greater,suffix:m"), "set_refraction_absorption_distance", "get_refraction_absorption_distance");

	ADD_GROUP("Detail", "detail_");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "detail_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_feature", "get_feature", FEATURE_DETAIL);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "detail_mask", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_DETAIL_MASK);
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_blend_mode", PROPERTY_HINT_ENUM, "Mix,Add,Subtract,Multiply"), "set_detail_blend_mode", "get_detail_blend_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "detail_uv_layer", PROPERTY_HINT_ENUM, "UV1,UV2"), "set_detail_uv", "get_detail_uv");
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "detail_albedo", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_DETAIL_ALBEDO);
	ADD_PROPERTYI(PropertyInfo(Variant::OBJECT, "detail_normal", PROPERTY_HINT_RESOURCE_TYPE, Texture2D::get_class_static()), "set_texture", "get_texture", TEXTURE_DETAIL_NORMAL);
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "detail_normal_rnm"), "set_detail_normal_rnm", "is_detail_normal_rnm"); // Mariner M4.

	ADD_GROUP("UV1", "uv1_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv1_scale", PROPERTY_HINT_LINK), "set_uv1_scale", "get_uv1_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv1_offset"), "set_uv1_offset", "get_uv1_offset");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv1_triplanar"), "set_flag", "get_flag", FLAG_UV1_USE_TRIPLANAR);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uv1_triplanar_sharpness", PROPERTY_HINT_EXP_EASING), "set_uv1_triplanar_blend_sharpness", "get_uv1_triplanar_blend_sharpness");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv1_world_triplanar"), "set_flag", "get_flag", FLAG_UV1_USE_WORLD_TRIPLANAR);

	ADD_GROUP("UV2", "uv2_");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv2_scale", PROPERTY_HINT_LINK), "set_uv2_scale", "get_uv2_scale");
	ADD_PROPERTY(PropertyInfo(Variant::VECTOR3, "uv2_offset"), "set_uv2_offset", "get_uv2_offset");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv2_triplanar"), "set_flag", "get_flag", FLAG_UV2_USE_TRIPLANAR);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "uv2_triplanar_sharpness", PROPERTY_HINT_EXP_EASING), "set_uv2_triplanar_blend_sharpness", "get_uv2_triplanar_blend_sharpness");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "uv2_world_triplanar"), "set_flag", "get_flag", FLAG_UV2_USE_WORLD_TRIPLANAR);

	ADD_GROUP("Sampling", "texture_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "texture_filter", PROPERTY_HINT_ENUM, "Nearest,Linear,Nearest Mipmap,Linear Mipmap,Nearest Mipmap Anisotropic,Linear Mipmap Anisotropic"), "set_texture_filter", "get_texture_filter");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "texture_repeat"), "set_flag", "get_flag", FLAG_USE_TEXTURE_REPEAT);

	ADD_GROUP("Shadows", "");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "disable_receive_shadows"), "set_flag", "get_flag", FLAG_DONT_RECEIVE_SHADOWS);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "shadow_to_opacity"), "set_flag", "get_flag", FLAG_USE_SHADOW_TO_OPACITY);

	ADD_GROUP("Billboard", "billboard_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "billboard_mode", PROPERTY_HINT_ENUM, "Disabled,Enabled,Y-Billboard,Particle Billboard"), "set_billboard_mode", "get_billboard_mode");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "billboard_keep_scale"), "set_flag", "get_flag", FLAG_BILLBOARD_KEEP_SCALE);

	ADD_GROUP("Particles Anim", "particles_anim_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "particles_anim_h_frames", PROPERTY_HINT_RANGE, "1,128,1"), "set_particles_anim_h_frames", "get_particles_anim_h_frames");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "particles_anim_v_frames", PROPERTY_HINT_RANGE, "1,128,1"), "set_particles_anim_v_frames", "get_particles_anim_v_frames");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "particles_anim_loop"), "set_particles_anim_loop", "get_particles_anim_loop");

	ADD_GROUP("Grow", "grow_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "grow", PROPERTY_HINT_GROUP_ENABLE), "set_grow_enabled", "is_grow_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "grow_amount", PROPERTY_HINT_RANGE, "-16,16,0.001,suffix:m"), "set_grow", "get_grow");

	ADD_GROUP("Transform", "");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "fixed_size"), "set_flag", "get_flag", FLAG_FIXED_SIZE);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_point_size"), "set_flag", "get_flag", FLAG_USE_POINT_SIZE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "point_size", PROPERTY_HINT_RANGE, "0.1,128,0.1,suffix:px"), "set_point_size", "get_point_size");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_particle_trails"), "set_flag", "get_flag", FLAG_PARTICLE_TRAILS_MODE);
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_z_clip_scale"), "set_flag", "get_flag", FLAG_USE_Z_CLIP_SCALE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "z_clip_scale", PROPERTY_HINT_RANGE, "0.01,1.0,0.01"), "set_z_clip_scale", "get_z_clip_scale");
	ADD_PROPERTYI(PropertyInfo(Variant::BOOL, "use_fov_override"), "set_flag", "get_flag", FLAG_USE_FOV_OVERRIDE);
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "fov_override", PROPERTY_HINT_RANGE, "1,179,0.1,degrees"), "set_fov_override", "get_fov_override");

	ADD_GROUP("Proximity Fade", "proximity_fade_");
	ADD_PROPERTY(PropertyInfo(Variant::BOOL, "proximity_fade_enabled", PROPERTY_HINT_GROUP_ENABLE), "set_proximity_fade_enabled", "is_proximity_fade_enabled");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "proximity_fade_distance", PROPERTY_HINT_RANGE, "0.01,4096,0.01,suffix:m"), "set_proximity_fade_distance", "get_proximity_fade_distance");

	ADD_GROUP("MSDF", "msdf_");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "msdf_pixel_range", PROPERTY_HINT_RANGE, "1,100,1"), "set_msdf_pixel_range", "get_msdf_pixel_range");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "msdf_outline_size", PROPERTY_HINT_RANGE, "0,250,1"), "set_msdf_outline_size", "get_msdf_outline_size");

	ADD_GROUP("Distance Fade", "distance_fade_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "distance_fade_mode", PROPERTY_HINT_ENUM, "Disabled,PixelAlpha,PixelDither,ObjectDither"), "set_distance_fade", "get_distance_fade");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_fade_min_distance", PROPERTY_HINT_RANGE, "0,4096,0.01,suffix:m"), "set_distance_fade_min_distance", "get_distance_fade_min_distance");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "distance_fade_max_distance", PROPERTY_HINT_RANGE, "0,4096,0.01,suffix:m"), "set_distance_fade_max_distance", "get_distance_fade_max_distance");

	ADD_GROUP("Stencil", "stencil_");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_mode", PROPERTY_HINT_ENUM, "Disabled,Outline,X-Ray,Custom"), "set_stencil_mode", "get_stencil_mode");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_flags", PROPERTY_HINT_FLAGS, "Read,Write,Write Depth Fail"), "set_stencil_flags", "get_stencil_flags");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_compare", PROPERTY_HINT_ENUM, "Always,Less,Equal,Less Or Equal,Greater,Not Equal,Greater Or Equal"), "set_stencil_compare", "get_stencil_compare");
	ADD_PROPERTY(PropertyInfo(Variant::INT, "stencil_reference", PROPERTY_HINT_RANGE, "0,255,1"), "set_stencil_reference", "get_stencil_reference");

	ADD_PROPERTY(PropertyInfo(Variant::COLOR, "stencil_color", PROPERTY_HINT_NONE), "set_stencil_effect_color", "get_stencil_effect_color");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "stencil_outline_thickness", PROPERTY_HINT_RANGE, "0,1,0.001,or_greater,suffix:m"), "set_stencil_effect_outline_thickness", "get_stencil_effect_outline_thickness");

	BIND_ENUM_CONSTANT(TEXTURE_ALBEDO);
	BIND_ENUM_CONSTANT(TEXTURE_METALLIC);
	BIND_ENUM_CONSTANT(TEXTURE_ROUGHNESS);
	BIND_ENUM_CONSTANT(TEXTURE_EMISSION);
	BIND_ENUM_CONSTANT(TEXTURE_NORMAL);
	BIND_ENUM_CONSTANT(TEXTURE_BENT_NORMAL);
	BIND_ENUM_CONSTANT(TEXTURE_SHEEN);
	BIND_ENUM_CONSTANT(TEXTURE_IRIDESCENCE);
	BIND_ENUM_CONSTANT(TEXTURE_INTERIOR); // Mariner G5a.
	BIND_ENUM_CONSTANT(TEXTURE_INTERIOR_MASK);
	BIND_ENUM_CONSTANT(TEXTURE_PLANAR_REFLECTION); // Mariner G6.
	BIND_ENUM_CONSTANT(TEXTURE_DISPLACEMENT); // Mariner G7.
	BIND_ENUM_CONSTANT(TEXTURE_ALPHA_MASK); // Mariner AA2.
	BIND_ENUM_CONSTANT(TEXTURE_RIM);
	BIND_ENUM_CONSTANT(TEXTURE_CLEARCOAT);
	BIND_ENUM_CONSTANT(TEXTURE_FLOWMAP);
	BIND_ENUM_CONSTANT(TEXTURE_AMBIENT_OCCLUSION);
	BIND_ENUM_CONSTANT(TEXTURE_HEIGHTMAP);
	BIND_ENUM_CONSTANT(TEXTURE_SUBSURFACE_SCATTERING);
	BIND_ENUM_CONSTANT(TEXTURE_SUBSURFACE_TRANSMITTANCE);
	BIND_ENUM_CONSTANT(TEXTURE_BACKLIGHT);
	BIND_ENUM_CONSTANT(TEXTURE_REFRACTION);
	BIND_ENUM_CONSTANT(TEXTURE_DETAIL_MASK);
	BIND_ENUM_CONSTANT(TEXTURE_DETAIL_ALBEDO);
	BIND_ENUM_CONSTANT(TEXTURE_DETAIL_NORMAL);
	BIND_ENUM_CONSTANT(TEXTURE_ORM);
	BIND_ENUM_CONSTANT(TEXTURE_MAX);

	BIND_ENUM_CONSTANT(TEXTURE_FILTER_NEAREST);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_LINEAR);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_NEAREST_WITH_MIPMAPS);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_LINEAR_WITH_MIPMAPS);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC);
	BIND_ENUM_CONSTANT(TEXTURE_FILTER_MAX);

	BIND_ENUM_CONSTANT(DETAIL_UV_1);
	BIND_ENUM_CONSTANT(DETAIL_UV_2);

	BIND_ENUM_CONSTANT(TRANSPARENCY_DISABLED);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_SCISSOR);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_HASH);
	BIND_ENUM_CONSTANT(TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
	BIND_ENUM_CONSTANT(TRANSPARENCY_MAX);

	BIND_ENUM_CONSTANT(SHADING_MODE_UNSHADED);
	BIND_ENUM_CONSTANT(SHADING_MODE_PER_PIXEL);
	BIND_ENUM_CONSTANT(SHADING_MODE_PER_VERTEX);
	BIND_ENUM_CONSTANT(SHADING_MODE_MAX);

	BIND_ENUM_CONSTANT(FEATURE_EMISSION);
	BIND_ENUM_CONSTANT(FEATURE_NORMAL_MAPPING);
	BIND_ENUM_CONSTANT(FEATURE_RIM);
	BIND_ENUM_CONSTANT(FEATURE_CLEARCOAT);
	BIND_ENUM_CONSTANT(FEATURE_ANISOTROPY);
	BIND_ENUM_CONSTANT(FEATURE_AMBIENT_OCCLUSION);
	BIND_ENUM_CONSTANT(FEATURE_HEIGHT_MAPPING);
	BIND_ENUM_CONSTANT(FEATURE_SUBSURFACE_SCATTERING);
	BIND_ENUM_CONSTANT(FEATURE_SUBSURFACE_TRANSMITTANCE);
	BIND_ENUM_CONSTANT(FEATURE_BACKLIGHT);
	BIND_ENUM_CONSTANT(FEATURE_REFRACTION);
	BIND_ENUM_CONSTANT(FEATURE_DETAIL);
	BIND_ENUM_CONSTANT(FEATURE_BENT_NORMAL_MAPPING);
	BIND_ENUM_CONSTANT(FEATURE_SHEEN);
	BIND_ENUM_CONSTANT(FEATURE_IRIDESCENCE);
	BIND_ENUM_CONSTANT(FEATURE_WETNESS); // Mariner G4.
	BIND_ENUM_CONSTANT(FEATURE_INTERIOR); // Mariner G5a.
	BIND_ENUM_CONSTANT(FEATURE_PLANAR_REFLECTION); // Mariner G6.
	BIND_ENUM_CONSTANT(FEATURE_DISPLACEMENT); // Mariner G7.
	BIND_ENUM_CONSTANT(FEATURE_SNOW); // Mariner G4b.
	BIND_ENUM_CONSTANT(FEATURE_MAX);

	BIND_ENUM_CONSTANT(METAL_ALUMINUM);
	BIND_ENUM_CONSTANT(METAL_BRASS);
	BIND_ENUM_CONSTANT(METAL_CHROMIUM);
	BIND_ENUM_CONSTANT(METAL_COPPER);
	BIND_ENUM_CONSTANT(METAL_GOLD);
	BIND_ENUM_CONSTANT(METAL_IRON);
	BIND_ENUM_CONSTANT(METAL_MERCURY);
	BIND_ENUM_CONSTANT(METAL_NICKEL);
	BIND_ENUM_CONSTANT(METAL_PALLADIUM);
	BIND_ENUM_CONSTANT(METAL_PLATINUM);
	BIND_ENUM_CONSTANT(METAL_SILVER);
	BIND_ENUM_CONSTANT(METAL_TITANIUM);
	BIND_ENUM_CONSTANT(METAL_ZINC);
	BIND_ENUM_CONSTANT(METAL_MAX);

	BIND_ENUM_CONSTANT(BLEND_MODE_MIX);
	BIND_ENUM_CONSTANT(BLEND_MODE_ADD);
	BIND_ENUM_CONSTANT(BLEND_MODE_SUB);
	BIND_ENUM_CONSTANT(BLEND_MODE_MUL);
	BIND_ENUM_CONSTANT(BLEND_MODE_PREMULT_ALPHA);

	BIND_ENUM_CONSTANT(ALPHA_ANTIALIASING_OFF);
	BIND_ENUM_CONSTANT(ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE);
	BIND_ENUM_CONSTANT(ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE);
	BIND_ENUM_CONSTANT(ALPHA_ANTIALIASING_FRINGE);

	BIND_ENUM_CONSTANT(ALPHA_MASK_ALBEDO);
	BIND_ENUM_CONSTANT(ALPHA_MASK_SDF);
	BIND_ENUM_CONSTANT(ALPHA_MASK_MSDF);

	BIND_ENUM_CONSTANT(DEPTH_DRAW_OPAQUE_ONLY);
	BIND_ENUM_CONSTANT(DEPTH_DRAW_ALWAYS);
	BIND_ENUM_CONSTANT(DEPTH_DRAW_DISABLED);

	BIND_ENUM_CONSTANT(DEPTH_TEST_DEFAULT);
	BIND_ENUM_CONSTANT(DEPTH_TEST_INVERTED);

	BIND_ENUM_CONSTANT(CULL_BACK);
	BIND_ENUM_CONSTANT(CULL_FRONT);
	BIND_ENUM_CONSTANT(CULL_DISABLED);

	BIND_ENUM_CONSTANT(FLAG_DISABLE_DEPTH_TEST);
	BIND_ENUM_CONSTANT(FLAG_ALBEDO_FROM_VERTEX_COLOR);
	BIND_ENUM_CONSTANT(FLAG_SRGB_VERTEX_COLOR);
	BIND_ENUM_CONSTANT(FLAG_USE_POINT_SIZE);
	BIND_ENUM_CONSTANT(FLAG_FIXED_SIZE);
	BIND_ENUM_CONSTANT(FLAG_BILLBOARD_KEEP_SCALE);
	BIND_ENUM_CONSTANT(FLAG_UV1_USE_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_UV2_USE_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_UV1_USE_WORLD_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_UV2_USE_WORLD_TRIPLANAR);
	BIND_ENUM_CONSTANT(FLAG_AO_ON_UV2);
	BIND_ENUM_CONSTANT(FLAG_EMISSION_ON_UV2);
	BIND_ENUM_CONSTANT(FLAG_ALBEDO_TEXTURE_FORCE_SRGB);
	BIND_ENUM_CONSTANT(FLAG_DONT_RECEIVE_SHADOWS);
	BIND_ENUM_CONSTANT(FLAG_DISABLE_AMBIENT_LIGHT);
	BIND_ENUM_CONSTANT(FLAG_USE_SHADOW_TO_OPACITY);
	BIND_ENUM_CONSTANT(FLAG_USE_TEXTURE_REPEAT);
	BIND_ENUM_CONSTANT(FLAG_INVERT_HEIGHTMAP);
	BIND_ENUM_CONSTANT(FLAG_SUBSURFACE_MODE_SKIN);
	BIND_ENUM_CONSTANT(FLAG_PARTICLE_TRAILS_MODE);
	BIND_ENUM_CONSTANT(FLAG_ALBEDO_TEXTURE_MSDF);
	BIND_ENUM_CONSTANT(FLAG_DISABLE_FOG);
	BIND_ENUM_CONSTANT(FLAG_DISABLE_SPECULAR_OCCLUSION);
	BIND_ENUM_CONSTANT(FLAG_USE_Z_CLIP_SCALE);
	BIND_ENUM_CONSTANT(FLAG_USE_FOV_OVERRIDE);
	BIND_ENUM_CONSTANT(FLAG_MAX);

	BIND_ENUM_CONSTANT(DIFFUSE_BURLEY);
	BIND_ENUM_CONSTANT(DIFFUSE_LAMBERT);
	BIND_ENUM_CONSTANT(DIFFUSE_LAMBERT_WRAP);
	BIND_ENUM_CONSTANT(DIFFUSE_TOON);

	BIND_ENUM_CONSTANT(SPECULAR_SCHLICK_GGX);
	BIND_ENUM_CONSTANT(SPECULAR_TOON);
	BIND_ENUM_CONSTANT(SPECULAR_DISABLED);

	BIND_ENUM_CONSTANT(BILLBOARD_DISABLED);
	BIND_ENUM_CONSTANT(BILLBOARD_ENABLED);
	BIND_ENUM_CONSTANT(BILLBOARD_FIXED_Y);
	BIND_ENUM_CONSTANT(BILLBOARD_PARTICLES);

	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_RED);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_GREEN);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_BLUE);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_ALPHA);
	BIND_ENUM_CONSTANT(TEXTURE_CHANNEL_GRAYSCALE);

	BIND_ENUM_CONSTANT(EMISSION_OP_ADD);
	BIND_ENUM_CONSTANT(EMISSION_OP_MULTIPLY);

	BIND_ENUM_CONSTANT(DISTANCE_FADE_DISABLED);
	BIND_ENUM_CONSTANT(DISTANCE_FADE_PIXEL_ALPHA);
	BIND_ENUM_CONSTANT(DISTANCE_FADE_PIXEL_DITHER);
	BIND_ENUM_CONSTANT(DISTANCE_FADE_OBJECT_DITHER);

	BIND_ENUM_CONSTANT(STENCIL_MODE_DISABLED);
	BIND_ENUM_CONSTANT(STENCIL_MODE_OUTLINE);
	BIND_ENUM_CONSTANT(STENCIL_MODE_XRAY);
	BIND_ENUM_CONSTANT(STENCIL_MODE_CUSTOM);

	BIND_ENUM_CONSTANT(STENCIL_FLAG_READ);
	BIND_ENUM_CONSTANT(STENCIL_FLAG_WRITE);
	BIND_ENUM_CONSTANT(STENCIL_FLAG_WRITE_DEPTH_FAIL);

	BIND_ENUM_CONSTANT(STENCIL_COMPARE_ALWAYS);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_LESS);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_EQUAL);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_LESS_OR_EQUAL);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_GREATER);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_NOT_EQUAL);
	BIND_ENUM_CONSTANT(STENCIL_COMPARE_GREATER_OR_EQUAL);
}

BaseMaterial3D::BaseMaterial3D(bool p_orm) :
		element(this) {
	orm = p_orm;
	// Initialize to the same values as the shader
	set_albedo(Color(1.0, 1.0, 1.0, 1.0));
	set_specular(0.5);
	set_roughness(1.0);
	set_metallic(0.0);
	set_emission(Color(0, 0, 0));
	set_emission_energy_multiplier(1.0);
	set_normal_scale(1);
	set_rim(1.0);
	set_rim_tint(0.5);
	set_clearcoat(1);
	set_clearcoat_roughness(0.5);
	set_anisotropy(0);
	set_heightmap_scale(5.0);
	set_subsurface_scattering_strength(0);
	set_backlight(Color(0, 0, 0));
	set_transmittance_color(Color(1, 1, 1, 1));
	set_transmittance_depth(0.1);
	set_transmittance_boost(0.0);
	set_refraction(0.05);
	// Mariner M6, F5, F6.
	set_wetness_amount(1.0); // Mariner G4.
	set_wetness_porosity(0.5);
	set_wetness_puddles(0.0);
	set_snow_amount(1.0); // Mariner G4b.
	set_snow_color(Color(0.9, 0.92, 0.95));
	set_snow_roughness(0.8);
	set_snow_max_slope(55.0);
	set_snow_relief(0.5);
	set_interior_rooms(Vector2(1, 1)); // Mariner G5a.
	set_interior_room_size(Vector3(4, 3, 4));
	set_interior_atlas_size(Vector2i(1, 1));
	set_interior_back_wall(0.5);
	set_interior_brightness(1.0);
	set_interior_lights(0.0);
	set_interior_lit_fraction(0.3);
	set_interior_light_color(Color(1.0, 0.8, 0.55));
	set_interior_curtains(0.0); // Mariner G5d.
	set_interior_blinds(0.0);
	set_interior_curtain_color(Color(0.75, 0.7, 0.6));
	set_planar_reflection_amount(1.0); // Mariner G6.
	set_planar_reflection_distortion(0.03);
	set_displacement_scale(0.1); // Mariner G7.
	set_displacement_midlevel(0.0);
	set_displacement_texture_lod(0.0);
	set_sheen_color(Color(1, 1, 1));
	set_sheen_roughness(0.5);
	set_iridescence(1.0);
	set_iridescence_ior(1.3);
	set_iridescence_thickness_min(100.0);
	set_iridescence_thickness_max(400.0);
	set_refraction_ior(1.5);
	set_refraction_thickness(0.05);
	set_refraction_absorption_color(Color(1, 1, 1));
	set_refraction_absorption_distance(1.0);
	set_point_size(1);
	set_uv1_offset(Vector3(0, 0, 0));
	set_uv1_scale(Vector3(1, 1, 1));
	set_uv1_triplanar_blend_sharpness(1);
	set_uv2_offset(Vector3(0, 0, 0));
	set_uv2_scale(Vector3(1, 1, 1));
	set_uv2_triplanar_blend_sharpness(1);
	set_billboard_mode(BILLBOARD_DISABLED);
	set_particles_anim_h_frames(1);
	set_particles_anim_v_frames(1);
	set_particles_anim_loop(false);

	set_transparency(TRANSPARENCY_DISABLED);
	set_alpha_antialiasing(ALPHA_ANTIALIASING_OFF);
	// Alpha scissor threshold of 0.5 matches the glTF specification and Label3D default.
	// <https://www.khronos.org/registry/glTF/specs/2.0/glTF-2.0.html#_material_alphacutoff>
	set_alpha_scissor_threshold(0.5);
	set_alpha_hash_scale(1.0);
	set_alpha_antialiasing_edge(0.3);

	set_proximity_fade_distance(1);
	set_distance_fade_min_distance(0);
	set_distance_fade_max_distance(10);

	set_ao_light_affect(0.0);

	set_metallic_texture_channel(TEXTURE_CHANNEL_RED);
	set_roughness_texture_channel(TEXTURE_CHANNEL_RED);
	set_ao_texture_channel(TEXTURE_CHANNEL_RED);
	set_refraction_texture_channel(TEXTURE_CHANNEL_RED);

	set_grow(0.0);

	set_msdf_pixel_range(4.0);
	set_msdf_outline_size(0.0);

	set_heightmap_deep_parallax_min_layers(8);
	set_heightmap_deep_parallax_max_layers(32);
	set_heightmap_self_shadow_strength(1.0); // Mariner E16.
	set_heightmap_deep_parallax_flip_tangent(false); //also sets binormal

	set_z_clip_scale(1.0);
	set_fov_override(75.0);

	set_stencil_mode(STENCIL_MODE_DISABLED);

	flags[FLAG_ALBEDO_TEXTURE_MSDF] = false;
	flags[FLAG_USE_TEXTURE_REPEAT] = true;

	current_key.invalid_key = 1;
}

BaseMaterial3D::~BaseMaterial3D() {
	ERR_FAIL_NULL(RS::get_singleton());

	{
		MutexLock lock(shader_map_mutex);
		if (shader_map.has(current_key)) {
			shader_map[current_key].users--;
			if (shader_map[current_key].users == 0) {
				// Deallocate shader which is no longer in use.
				RS::get_singleton()->free_rid(shader_map[current_key].shader);
				shader_map.erase(current_key);
			}
		}
	}

	if (_get_material().is_valid()) {
		RS::get_singleton()->material_set_shader(_get_material(), RID());
	}
}

//////////////////////

#ifndef DISABLE_DEPRECATED
// Kept for compatibility from 3.x to 4.0.
bool StandardMaterial3D::_set(const StringName &p_name, const Variant &p_value) {
	if (p_name == "flags_transparent") {
		bool transparent = p_value;
		if (transparent) {
			set_transparency(TRANSPARENCY_ALPHA);
		}
		return true;
	} else if (p_name == "flags_unshaded") {
		bool unshaded = p_value;
		if (unshaded) {
			set_shading_mode(SHADING_MODE_UNSHADED);
		}
		return true;
	} else if (p_name == "flags_vertex_lighting") {
		bool vertex_lit = p_value;
		if (vertex_lit && get_shading_mode() != SHADING_MODE_UNSHADED) {
			set_shading_mode(SHADING_MODE_PER_VERTEX);
		}
		return true;
	} else if (p_name == "params_use_alpha_scissor") {
		bool use_scissor = p_value;
		if (use_scissor) {
			set_transparency(TRANSPARENCY_ALPHA_SCISSOR);
		}
		return true;
	} else if (p_name == "params_use_alpha_hash") {
		bool use_hash = p_value;
		if (use_hash) {
			set_transparency(TRANSPARENCY_ALPHA_HASH);
		}
		return true;
	} else if (p_name == "params_depth_draw_mode") {
		int mode = p_value;
		if (mode == 3) {
			set_transparency(TRANSPARENCY_ALPHA_DEPTH_PRE_PASS);
		}
		return true;
	} else if (p_name == "depth_enabled") {
		bool enabled = p_value;
		if (enabled) {
			set_feature(FEATURE_HEIGHT_MAPPING, true);
			set_flag(FLAG_INVERT_HEIGHTMAP, true);
		}
		return true;
	} else {
		static const Pair<const char *, const char *> remaps[] = {
			{ "flags_use_shadow_to_opacity", "shadow_to_opacity" },
			{ "flags_use_shadow_to_opacity", "shadow_to_opacity" },
			{ "flags_no_depth_test", "no_depth_test" },
			{ "flags_use_point_size", "use_point_size" },
			{ "flags_fixed_size", "fixed_size" },
			{ "flags_albedo_tex_force_srgb", "albedo_texture_force_srgb" },
			{ "flags_do_not_receive_shadows", "disable_receive_shadows" },
			{ "flags_disable_ambient_light", "disable_ambient_light" },
			{ "params_diffuse_mode", "diffuse_mode" },
			{ "params_specular_mode", "specular_mode" },
			{ "params_blend_mode", "blend_mode" },
			{ "params_cull_mode", "cull_mode" },
			{ "params_depth_draw_mode", "params_depth_draw_mode" },
			{ "params_point_size", "point_size" },
			{ "params_billboard_mode", "billboard_mode" },
			{ "params_billboard_keep_scale", "billboard_keep_scale" },
			{ "params_grow", "grow" },
			{ "params_grow_amount", "grow_amount" },
			{ "params_alpha_scissor_threshold", "alpha_scissor_threshold" },
			{ "params_alpha_hash_scale", "alpha_hash_scale" },
			{ "params_alpha_antialiasing_edge", "alpha_antialiasing_edge" },

			{ "depth_scale", "heightmap_scale" },
			{ "depth_deep_parallax", "heightmap_deep_parallax" },
			{ "depth_min_layers", "heightmap_min_layers" },
			{ "depth_max_layers", "heightmap_max_layers" },
			{ "depth_flip_tangent", "heightmap_flip_tangent" },
			{ "depth_flip_binormal", "heightmap_flip_binormal" },
			{ "depth_texture", "heightmap_texture" },

			{ "emission_energy", "emission_energy_multiplier" },

			{ nullptr, nullptr },
		};

		int idx = 0;
		while (remaps[idx].first) {
			if (p_name == remaps[idx].first) {
				set(remaps[idx].second, p_value);
				return true;
			}
			idx++;
		}

		WARN_PRINT("Godot 3.x SpatialMaterial remapped parameter not found: " + String(p_name));
		return true;
	}
}

#endif // DISABLE_DEPRECATED

///////////////////////
