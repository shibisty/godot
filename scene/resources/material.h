/**************************************************************************/
/*  material.h                                                            */
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

#include "core/io/image.h"
#include "core/io/resource.h"
#include "core/templates/self_list.h"
#include "scene/resources/shader.h"
#include "servers/rendering/rendering_server_enums.h"

class Material : public Resource {
	GDCLASS(Material, Resource);
	RES_BASE_EXTENSION("material")
	OBJ_SAVE_TYPE(Material);

	mutable RID material;
	Ref<Material> next_pass;
	int render_priority;

	enum {
		INIT_STATE_UNINITIALIZED,
		INIT_STATE_INITIALIZING,
		INIT_STATE_READY,
	} init_state = INIT_STATE_UNINITIALIZED;

	void inspect_native_shader_code();

protected:
	_FORCE_INLINE_ void _set_material(RID p_material) const { material = p_material; }
	_FORCE_INLINE_ RID _get_material() const { return material; }
	static void _bind_methods();
	virtual bool _can_do_next_pass() const;
	virtual bool _can_use_render_priority() const;

	void _validate_property(PropertyInfo &p_property) const;

	void _mark_ready();
	void _mark_initialized(const Callable &p_add_to_dirty_list, const Callable &p_update_shader);

	GDVIRTUAL0RC_REQUIRED(RID, _get_shader_rid)
	GDVIRTUAL0RC_REQUIRED(Shader::Mode, _get_shader_mode)
	GDVIRTUAL0RC(bool, _can_do_next_pass)
	GDVIRTUAL0RC(bool, _can_use_render_priority)
public:
	enum {
		RENDER_PRIORITY_MAX = RSE::MATERIAL_RENDER_PRIORITY_MAX,
		RENDER_PRIORITY_MIN = RSE::MATERIAL_RENDER_PRIORITY_MIN,
	};

	bool _is_initialized() { return init_state == INIT_STATE_READY; }

	void set_next_pass(const Ref<Material> &p_pass);
	Ref<Material> get_next_pass() const;

	void set_render_priority(int p_priority);
	int get_render_priority() const;

	virtual RID get_rid() const override;
	virtual RID get_shader_rid() const;
	virtual Shader::Mode get_shader_mode() const;

	virtual Ref<Resource> create_placeholder() const;

	Material();
	virtual ~Material();
};

class ShaderMaterial : public Material {
	GDCLASS(ShaderMaterial, Material);
	Ref<Shader> shader;

	mutable HashMap<StringName, StringName> remap_cache;
	mutable HashMap<StringName, Variant> param_cache;
	mutable Mutex material_rid_mutex;

protected:
	bool _set(const StringName &p_name, const Variant &p_value);
	bool _get(const StringName &p_name, Variant &r_ret) const;
	void _get_property_list(List<PropertyInfo> *p_list) const;
	bool _property_can_revert(const StringName &p_name) const;
	bool _property_get_revert(const StringName &p_name, Variant &r_property) const;

	static void _bind_methods();

#ifdef TOOLS_ENABLED
	void get_argument_options(const StringName &p_function, int p_idx, List<String> *r_options) const override;
#endif

	virtual bool _can_do_next_pass() const override;
	virtual bool _can_use_render_priority() const override;

	void _shader_changed();
	void _check_material_rid() const;

public:
	void set_shader(const Ref<Shader> &p_shader);
	Ref<Shader> get_shader() const;

	void set_shader_parameter(const StringName &p_param, const Variant &p_value);
	Variant get_shader_parameter(const StringName &p_param) const;

	virtual Shader::Mode get_shader_mode() const override;

	virtual RID get_rid() const override;
	virtual RID get_shader_rid() const override;

	ShaderMaterial();
	~ShaderMaterial();
};

class StandardMaterial3D;

class BaseMaterial3D : public Material {
	GDCLASS(BaseMaterial3D, Material);

private:
	mutable Mutex material_rid_mutex;

public:
	enum TextureParam {
		TEXTURE_ALBEDO,
		TEXTURE_METALLIC,
		TEXTURE_ROUGHNESS,
		TEXTURE_EMISSION,
		TEXTURE_NORMAL,
		TEXTURE_RIM,
		TEXTURE_CLEARCOAT,
		TEXTURE_FLOWMAP,
		TEXTURE_AMBIENT_OCCLUSION,
		TEXTURE_HEIGHTMAP,
		TEXTURE_SUBSURFACE_SCATTERING,
		TEXTURE_SUBSURFACE_TRANSMITTANCE,
		TEXTURE_BACKLIGHT,
		TEXTURE_REFRACTION,
		TEXTURE_DETAIL_MASK,
		TEXTURE_DETAIL_ALBEDO,
		TEXTURE_DETAIL_NORMAL,
		TEXTURE_ORM,
		TEXTURE_BENT_NORMAL,
		TEXTURE_SHEEN, // Mariner M6.
		TEXTURE_IRIDESCENCE, // Mariner F5.
		TEXTURE_INTERIOR, // Mariner G5a.
		TEXTURE_INTERIOR_MASK,
		TEXTURE_PLANAR_REFLECTION, // Mariner G6.
		TEXTURE_DISPLACEMENT, // Mariner G7.
		TEXTURE_ALPHA_MASK, // Mariner AA2.
		TEXTURE_MAX
	};

	enum TextureFilter {
		TEXTURE_FILTER_NEAREST,
		TEXTURE_FILTER_LINEAR,
		TEXTURE_FILTER_NEAREST_WITH_MIPMAPS,
		TEXTURE_FILTER_LINEAR_WITH_MIPMAPS,
		TEXTURE_FILTER_NEAREST_WITH_MIPMAPS_ANISOTROPIC,
		TEXTURE_FILTER_LINEAR_WITH_MIPMAPS_ANISOTROPIC,
		TEXTURE_FILTER_MAX
	};

	enum DetailUV {
		DETAIL_UV_1,
		DETAIL_UV_2,
		DETAIL_UV_MAX
	};

	enum Transparency {
		TRANSPARENCY_DISABLED,
		TRANSPARENCY_ALPHA,
		TRANSPARENCY_ALPHA_SCISSOR,
		TRANSPARENCY_ALPHA_HASH,
		TRANSPARENCY_ALPHA_DEPTH_PRE_PASS,
		TRANSPARENCY_MAX,
	};

	enum AlphaAntiAliasing {
		ALPHA_ANTIALIASING_OFF,
		ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE,
		ALPHA_ANTIALIASING_ALPHA_TO_COVERAGE_AND_TO_ONE,
		ALPHA_ANTIALIASING_FRINGE, // Mariner AA: analytic edge of the alpha scissor (Forward+), no MSAA.
		ALPHA_ANTIALIASING_MAX
	};

	// Mariner AA2: what the alpha scissor / hash / blend tests - the albedo alpha (coverage) or a distance field in
	// alpha_mask_texture (0.5 on the contour): one channel (SDF) or the median of three (MSDF, sharp corners).
	enum AlphaMaskMode {
		ALPHA_MASK_ALBEDO,
		ALPHA_MASK_SDF,
		ALPHA_MASK_MSDF,
		ALPHA_MASK_MAX
	};

	enum ShadingMode {
		SHADING_MODE_UNSHADED,
		SHADING_MODE_PER_PIXEL,
		SHADING_MODE_PER_VERTEX,
		SHADING_MODE_MAX
	};

	// Mariner M8: measured reflectance at normal incidence (F0) of pure metals, see get_metal_albedo().
	enum Metal {
		METAL_ALUMINUM,
		METAL_BRASS,
		METAL_CHROMIUM,
		METAL_COPPER,
		METAL_GOLD,
		METAL_IRON,
		METAL_MERCURY,
		METAL_NICKEL,
		METAL_PALLADIUM,
		METAL_PLATINUM,
		METAL_SILVER,
		METAL_TITANIUM,
		METAL_ZINC,
		METAL_MAX
	};

	enum Feature {
		FEATURE_EMISSION,
		FEATURE_NORMAL_MAPPING,
		FEATURE_RIM,
		FEATURE_CLEARCOAT,
		FEATURE_ANISOTROPY,
		FEATURE_AMBIENT_OCCLUSION,
		FEATURE_HEIGHT_MAPPING,
		FEATURE_SUBSURFACE_SCATTERING,
		FEATURE_SUBSURFACE_TRANSMITTANCE,
		FEATURE_BACKLIGHT,
		FEATURE_REFRACTION,
		FEATURE_DETAIL,
		FEATURE_BENT_NORMAL_MAPPING,
		FEATURE_SHEEN, // Mariner M6.
		FEATURE_IRIDESCENCE, // Mariner F5.
		FEATURE_WETNESS, // Mariner G4.
		FEATURE_INTERIOR, // Mariner G5a.
		FEATURE_PLANAR_REFLECTION, // Mariner G6.
		FEATURE_DISPLACEMENT, // Mariner G7.
		FEATURE_SNOW, // Mariner G4b.
		FEATURE_MAX
	};

	enum BlendMode {
		BLEND_MODE_MIX,
		BLEND_MODE_ADD,
		BLEND_MODE_SUB,
		BLEND_MODE_MUL,
		BLEND_MODE_PREMULT_ALPHA,
		BLEND_MODE_MAX
	};

	enum DepthDrawMode {
		DEPTH_DRAW_OPAQUE_ONLY,
		DEPTH_DRAW_ALWAYS,
		DEPTH_DRAW_DISABLED,
		DEPTH_DRAW_MAX
	};

	enum DepthTest {
		DEPTH_TEST_DEFAULT,
		DEPTH_TEST_INVERTED,
		DEPTH_TEST_MAX
	};

	enum CullMode {
		CULL_BACK,
		CULL_FRONT,
		CULL_DISABLED,
		CULL_MAX
	};

	enum Flags {
		FLAG_DISABLE_DEPTH_TEST,
		FLAG_ALBEDO_FROM_VERTEX_COLOR,
		FLAG_SRGB_VERTEX_COLOR,
		FLAG_USE_POINT_SIZE,
		FLAG_FIXED_SIZE,
		FLAG_BILLBOARD_KEEP_SCALE,
		FLAG_UV1_USE_TRIPLANAR,
		FLAG_UV2_USE_TRIPLANAR,
		FLAG_UV1_USE_WORLD_TRIPLANAR,
		FLAG_UV2_USE_WORLD_TRIPLANAR,
		FLAG_AO_ON_UV2,
		FLAG_EMISSION_ON_UV2,
		FLAG_ALBEDO_TEXTURE_FORCE_SRGB,
		FLAG_DONT_RECEIVE_SHADOWS,
		FLAG_DISABLE_AMBIENT_LIGHT,
		FLAG_USE_SHADOW_TO_OPACITY,
		FLAG_USE_TEXTURE_REPEAT,
		FLAG_INVERT_HEIGHTMAP,
		FLAG_SUBSURFACE_MODE_SKIN,
		FLAG_PARTICLE_TRAILS_MODE,
		FLAG_ALBEDO_TEXTURE_MSDF,
		FLAG_DISABLE_FOG,
		FLAG_DISABLE_SPECULAR_OCCLUSION,
		FLAG_USE_Z_CLIP_SCALE,
		FLAG_USE_FOV_OVERRIDE,
		FLAG_MAX
	};

	enum DiffuseMode {
		DIFFUSE_BURLEY,
		DIFFUSE_LAMBERT,
		DIFFUSE_LAMBERT_WRAP,
		DIFFUSE_TOON,
		DIFFUSE_MAX
	};

	enum SpecularMode {
		SPECULAR_SCHLICK_GGX,
		SPECULAR_TOON,
		SPECULAR_DISABLED,
		SPECULAR_MAX
	};

	enum BillboardMode {
		BILLBOARD_DISABLED,
		BILLBOARD_ENABLED,
		BILLBOARD_FIXED_Y,
		BILLBOARD_PARTICLES,
		BILLBOARD_MAX
	};

	enum TextureChannel {
		TEXTURE_CHANNEL_RED,
		TEXTURE_CHANNEL_GREEN,
		TEXTURE_CHANNEL_BLUE,
		TEXTURE_CHANNEL_ALPHA,
		TEXTURE_CHANNEL_GRAYSCALE,
		TEXTURE_CHANNEL_MAX
	};

	enum EmissionOperator {
		EMISSION_OP_ADD,
		EMISSION_OP_MULTIPLY,
		EMISSION_OP_MAX
	};

	enum DistanceFadeMode {
		DISTANCE_FADE_DISABLED,
		DISTANCE_FADE_PIXEL_ALPHA,
		DISTANCE_FADE_PIXEL_DITHER,
		DISTANCE_FADE_OBJECT_DITHER,
		DISTANCE_FADE_MAX
	};

	enum StencilMode {
		STENCIL_MODE_DISABLED,
		STENCIL_MODE_OUTLINE,
		STENCIL_MODE_XRAY,
		STENCIL_MODE_CUSTOM,
		STENCIL_MODE_MAX // Not an actual mode, just the amount of modes.
	};

	enum StencilFlags {
		STENCIL_FLAG_READ = 1,
		STENCIL_FLAG_WRITE = 2,
		STENCIL_FLAG_WRITE_DEPTH_FAIL = 4,

		STENCIL_FLAG_NUM_BITS = 3 // Not an actual mode, just the amount of bits.
	};

	enum StencilCompare {
		STENCIL_COMPARE_ALWAYS,
		STENCIL_COMPARE_LESS,
		STENCIL_COMPARE_EQUAL,
		STENCIL_COMPARE_LESS_OR_EQUAL,
		STENCIL_COMPARE_GREATER,
		STENCIL_COMPARE_NOT_EQUAL,
		STENCIL_COMPARE_GREATER_OR_EQUAL,
		STENCIL_COMPARE_MAX // Not an actual operator, just the amount of operators.
	};

private:
	struct MaterialKey {
		// enum values
		uint64_t texture_filter : Math::get_num_bits(TEXTURE_FILTER_MAX - 1);
		uint64_t detail_uv : Math::get_num_bits(DETAIL_UV_MAX - 1);
		uint64_t transparency : Math::get_num_bits(TRANSPARENCY_MAX - 1);
		uint64_t alpha_antialiasing_mode : Math::get_num_bits(ALPHA_ANTIALIASING_MAX - 1);
		uint64_t alpha_mask_mode : Math::get_num_bits(ALPHA_MASK_MAX - 1); // Mariner AA2.
		uint64_t shading_mode : Math::get_num_bits(SHADING_MODE_MAX - 1);
		uint64_t blend_mode : Math::get_num_bits(BLEND_MODE_MAX - 1);
		uint64_t depth_draw_mode : Math::get_num_bits(DEPTH_DRAW_MAX - 1);
		uint64_t depth_test : Math::get_num_bits(DEPTH_TEST_MAX - 1);
		uint64_t cull_mode : Math::get_num_bits(CULL_MAX - 1);
		uint64_t diffuse_mode : Math::get_num_bits(DIFFUSE_MAX - 1);
		uint64_t specular_mode : Math::get_num_bits(SPECULAR_MAX - 1);
		uint64_t billboard_mode : Math::get_num_bits(BILLBOARD_MAX - 1);
		uint64_t detail_blend_mode : Math::get_num_bits(BLEND_MODE_MAX - 1);
		uint64_t roughness_channel : Math::get_num_bits(TEXTURE_CHANNEL_MAX - 1);
		uint64_t emission_op : Math::get_num_bits(EMISSION_OP_MAX - 1);
		uint64_t distance_fade : Math::get_num_bits(DISTANCE_FADE_MAX - 1);

		// stencil
		uint64_t stencil_mode : Math::get_num_bits(STENCIL_MODE_MAX - 1);
		uint64_t stencil_flags : STENCIL_FLAG_NUM_BITS;
		uint64_t stencil_compare : Math::get_num_bits(STENCIL_COMPARE_MAX - 1);
		uint64_t stencil_reference : 8;

		// booleans
		uint64_t invalid_key : 1;
		uint64_t deep_parallax : 1;
		uint64_t grow : 1;
		uint64_t proximity_fade : 1;
		uint64_t orm : 1;
		uint64_t heightmap_self_shadow : 1; // Mariner E16.
		uint64_t heightmap_depth_offset : 1;
		uint64_t heightmap_impostor : 1; // Mariner G5b.
		uint64_t heightmap_impostor_hull : 1; // Mariner G5c.
		uint64_t refraction_physical : 1; // Mariner F6.
		uint64_t displacement_normal : 1; // Mariner G7.
		uint64_t detail_normal_rnm : 1; // Mariner M4.

		// flag bitfield
		uint32_t feature_mask;
		uint32_t flags;

		// Mariner AA3: the first word of bit fields is full (24 bytes; hashed and compared as bytes, zeroed by memset).
		uint64_t wire_antialiasing : 1;

		MaterialKey() {
			memset(this, 0, sizeof(MaterialKey));
		}

		static uint32_t hash(const MaterialKey &p_key) {
			return hash_djb2_buffer((const uint8_t *)&p_key, sizeof(MaterialKey));
		}
		bool operator==(const MaterialKey &p_key) const {
			return memcmp(this, &p_key, sizeof(MaterialKey)) == 0;
		}

		bool operator<(const MaterialKey &p_key) const {
			return memcmp(this, &p_key, sizeof(MaterialKey)) < 0;
		}
	};

	struct ShaderData {
		RID shader;
		int users = 0;
	};

	static HashMap<MaterialKey, ShaderData, MaterialKey> shader_map;
	static Mutex shader_map_mutex;

	MaterialKey current_key;

	_FORCE_INLINE_ MaterialKey _compute_key() const {
		MaterialKey mk;

		mk.detail_uv = detail_uv;
		mk.blend_mode = blend_mode;
		mk.depth_draw_mode = depth_draw_mode;
		mk.depth_test = depth_test;
		mk.cull_mode = cull_mode;
		mk.texture_filter = texture_filter;
		mk.transparency = transparency;
		mk.shading_mode = shading_mode;
		mk.roughness_channel = roughness_texture_channel;
		mk.detail_blend_mode = detail_blend_mode;
		mk.diffuse_mode = diffuse_mode;
		mk.specular_mode = specular_mode;
		mk.billboard_mode = billboard_mode;
		mk.deep_parallax = deep_parallax;
		mk.grow = grow_enabled;
		mk.wire_antialiasing = wire_antialiasing;
		mk.proximity_fade = proximity_fade_enabled;
		mk.distance_fade = distance_fade;
		mk.emission_op = emission_op;
		mk.alpha_antialiasing_mode = alpha_antialiasing_mode;
		mk.alpha_mask_mode = alpha_mask_mode;
		mk.orm = orm;
		mk.heightmap_self_shadow = heightmap_self_shadow;
		mk.heightmap_depth_offset = heightmap_depth_offset;
		mk.heightmap_impostor = heightmap_impostor;
		mk.heightmap_impostor_hull = heightmap_impostor_faces.size() >= IMPOSTOR_FACE_DATA_SIZE;
		mk.refraction_physical = refraction_physical;
		mk.displacement_normal = displacement_normal;
		mk.detail_normal_rnm = detail_normal_rnm;

		mk.stencil_mode = stencil_mode;
		mk.stencil_flags = stencil_flags;
		mk.stencil_compare = stencil_compare;
		mk.stencil_reference = stencil_reference;

		for (int i = 0; i < FEATURE_MAX; i++) {
			if (features[i]) {
				mk.feature_mask |= ((uint64_t)1 << i);
			}
		}

		for (int i = 0; i < FLAG_MAX; i++) {
			if (flags[i]) {
				mk.flags |= ((uint64_t)1 << i);
			}
		}

		return mk;
	}

	struct ShaderNames {
		StringName albedo;
		StringName specular;
		StringName metallic;
		StringName roughness;
		StringName emission;
		StringName emission_energy;
		StringName normal_scale;
		StringName rim;
		StringName rim_tint;
		StringName clearcoat;
		StringName clearcoat_roughness;
		StringName anisotropy;
		StringName heightmap_scale;
		StringName subsurface_scattering_strength;
		StringName transmittance_color;
		StringName transmittance_depth;
		StringName transmittance_boost;
		StringName backlight;
		StringName refraction;
		StringName point_size;
		StringName uv1_scale;
		StringName uv1_offset;
		StringName uv2_scale;
		StringName uv2_offset;
		StringName particles_anim_h_frames;
		StringName particles_anim_v_frames;
		StringName particles_anim_loop;
		StringName heightmap_min_layers;
		StringName heightmap_max_layers;
		StringName heightmap_flip;
		StringName heightmap_self_shadow_strength; // Mariner E16.
		// Mariner M6, F5, F6.
		StringName sheen_color;
		StringName sheen_roughness;
		StringName wetness_amount; // Mariner G4.
		StringName wetness_porosity;
		StringName wetness_puddles;
		StringName snow_amount; // Mariner G4b.
		StringName snow_color;
		StringName snow_roughness;
		StringName snow_max_slope;
		StringName snow_relief;
		StringName impostor_faces; // Mariner G5c.
		StringName planar_reflection_amount; // Mariner G6.
		StringName planar_reflection_distortion;
		StringName displacement_scale; // Mariner G7.
		StringName displacement_midlevel;
		StringName displacement_texture_lod;
		StringName interior_rooms; // Mariner G5a.
		StringName interior_room_size;
		StringName interior_atlas_size;
		StringName interior_back_wall;
		StringName interior_brightness;
		StringName interior_lights;
		StringName interior_lit_fraction;
		StringName interior_light_color;
		StringName interior_curtains; // Mariner G5d.
		StringName interior_blinds;
		StringName interior_curtain_color;
		StringName iridescence;
		StringName iridescence_ior;
		StringName iridescence_thickness_min;
		StringName iridescence_thickness_max;
		StringName refraction_ior;
		StringName refraction_thickness;
		StringName refraction_absorption_color;
		StringName refraction_absorption_distance;
		StringName uv1_blend_sharpness;
		StringName uv2_blend_sharpness;
		StringName grow;
		StringName proximity_fade_distance;
		StringName msdf_pixel_range;
		StringName msdf_outline_size;
		StringName distance_fade_min;
		StringName distance_fade_max;
		StringName ao_light_affect;

		StringName metallic_texture_channel;
		StringName ao_texture_channel;
		StringName clearcoat_texture_channel;
		StringName rim_texture_channel;
		StringName heightmap_texture_channel;
		StringName refraction_texture_channel;

		StringName texture_names[TEXTURE_MAX];

		StringName alpha_scissor_threshold;
		StringName alpha_hash_scale;

		StringName alpha_antialiasing_edge;
		StringName albedo_texture_size;
		StringName z_clip_scale;
		StringName fov_override;
	};

	static Mutex material_mutex;
	static SelfList<BaseMaterial3D>::List dirty_materials;
	static ShaderNames *shader_names;

	SelfList<BaseMaterial3D> element;

	void _update_shader();
	_FORCE_INLINE_ void _queue_shader_change();
	void _check_material_rid();
	void _material_set_param(const StringName &p_name, const Variant &p_value);

	bool orm;
	RID shader_rid;
	HashMap<StringName, Variant> pending_params;

	Color albedo;
	float specular = 0.0f;
	float metallic = 0.0f;
	float roughness = 0.0f;
	Color emission;
	float emission_energy_multiplier = 1.0f;
	float emission_intensity = 1000.0f; // In nits, equivalent to indoor lighting.
	float normal_scale = 0.0f;
	float rim = 0.0f;
	float rim_tint = 0.0f;
	float clearcoat = 0.0f;
	float clearcoat_roughness = 0.0f;
	float anisotropy = 0.0f;
	float heightmap_scale = 0.0f;
	float subsurface_scattering_strength = 0.0f;
	Color transmittance_color;
	float transmittance_depth = 0.0f;
	float transmittance_boost = 0.0f;

	Color backlight;
	float refraction = 0.0f;
	float point_size = 0.0f;
	float alpha_scissor_threshold = 0.0f;
	float alpha_hash_scale = 0.0f;
	float alpha_antialiasing_edge = 0.0f;
	bool grow_enabled = false;
	bool wire_antialiasing = false; // Mariner AA3.
	float ao_light_affect = 0.0f;
	float grow = 0.0f;
	int particles_anim_h_frames = 0;
	int particles_anim_v_frames = 0;
	bool particles_anim_loop = false;
	Transparency transparency = TRANSPARENCY_DISABLED;
	ShadingMode shading_mode = SHADING_MODE_PER_PIXEL;

	TextureFilter texture_filter = TEXTURE_FILTER_LINEAR_WITH_MIPMAPS;

	Vector3 uv1_scale;
	Vector3 uv1_offset;
	float uv1_triplanar_sharpness = 0.0f;

	Vector3 uv2_scale;
	Vector3 uv2_offset;
	float uv2_triplanar_sharpness = 0.0f;

	DetailUV detail_uv = DETAIL_UV_1;

	bool deep_parallax = false;
	int deep_parallax_min_layers = 0;
	int deep_parallax_max_layers = 0;
	bool heightmap_parallax_flip_tangent = false;
	bool heightmap_parallax_flip_binormal = false;
	// Mariner E16: self-shadowing towards the main light and depth of the relief point.
	bool heightmap_self_shadow = false;
	float heightmap_self_shadow_strength = 1.0;
	bool heightmap_depth_offset = false;
	bool heightmap_impostor = false; // Mariner G5b.
	PackedVector4Array heightmap_impostor_faces; // Mariner G5c.
	float planar_reflection_amount = 1.0; // Mariner G6.
	float planar_reflection_distortion = 0.03;
	float displacement_scale = 0.1; // Mariner G7.
	float displacement_midlevel = 0.0;
	float displacement_texture_lod = 0.0;
	bool displacement_normal = true;
	// Mariner M6, F5, F6, M4.
	Color sheen_color = Color(1, 1, 1);
	float sheen_roughness = 0.5;
	float wetness_amount = 1.0; // Mariner G4.
	float wetness_porosity = 0.5;
	float wetness_puddles = 0.0;
	float snow_amount = 1.0; // Mariner G4b.
	Color snow_color = Color(0.9, 0.92, 0.95);
	float snow_roughness = 0.8;
	float snow_max_slope = 55.0;
	float snow_relief = 0.5;
	Vector2 interior_rooms = Vector2(1, 1); // Mariner G5a.
	Vector3 interior_room_size = Vector3(4, 3, 4);
	Vector2i interior_atlas_size = Vector2i(1, 1);
	float interior_back_wall = 0.5;
	float interior_brightness = 1.0;
	float interior_lights = 0.0;
	float interior_lit_fraction = 0.3;
	Color interior_light_color = Color(1.0, 0.8, 0.55);
	float interior_curtains = 0.0; // Mariner G5d.
	float interior_blinds = 0.0;
	Color interior_curtain_color = Color(0.75, 0.7, 0.6);
	float iridescence = 1.0;
	float iridescence_ior = 1.3;
	float iridescence_thickness_min = 100.0;
	float iridescence_thickness_max = 400.0;
	float refraction_ior = 1.5;
	float refraction_thickness = 0.05;
	Color refraction_absorption_color = Color(1, 1, 1);
	float refraction_absorption_distance = 1.0;
	bool refraction_physical = false;
	bool detail_normal_rnm = false;

	bool proximity_fade_enabled = false;
	float proximity_fade_distance = 0.0f;

	float msdf_pixel_range = 4.f;
	float msdf_outline_size = 0.f;

	DistanceFadeMode distance_fade = DISTANCE_FADE_DISABLED;
	float distance_fade_max_distance = 0.0f;
	float distance_fade_min_distance = 0.0f;

	BlendMode blend_mode = BLEND_MODE_MIX;
	BlendMode detail_blend_mode = BLEND_MODE_MIX;
	DepthDrawMode depth_draw_mode = DEPTH_DRAW_OPAQUE_ONLY;
	DepthTest depth_test = DEPTH_TEST_DEFAULT;
	CullMode cull_mode = CULL_BACK;
	bool flags[FLAG_MAX] = {};
	SpecularMode specular_mode = SPECULAR_SCHLICK_GGX;
	DiffuseMode diffuse_mode = DIFFUSE_BURLEY;
	BillboardMode billboard_mode;
	EmissionOperator emission_op = EMISSION_OP_ADD;

	TextureChannel metallic_texture_channel;
	TextureChannel roughness_texture_channel;
	TextureChannel ao_texture_channel;
	TextureChannel refraction_texture_channel;

	AlphaAntiAliasing alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF;
	AlphaMaskMode alpha_mask_mode = ALPHA_MASK_ALBEDO; // Mariner AA2.

	float z_clip_scale = 1.0;
	float fov_override = 75.0;

	StencilMode stencil_mode = STENCIL_MODE_DISABLED;
	int stencil_flags = 0;
	StencilCompare stencil_compare = STENCIL_COMPARE_ALWAYS;
	int stencil_reference = 1;

	Color stencil_effect_color;
	float stencil_effect_outline_thickness = 0.01f;

	bool features[FEATURE_MAX] = {};

	Ref<Texture2D> textures[TEXTURE_MAX];

	void _prepare_stencil_effect();
	Ref<BaseMaterial3D> _get_stencil_next_pass() const;

	static HashMap<uint64_t, Ref<StandardMaterial3D>> materials_for_2d; //used by Sprite3D, Label3D and other stuff

protected:
	static void _bind_methods();
	void _validate_property(PropertyInfo &p_property) const;
	virtual bool _can_do_next_pass() const override { return true; }
	virtual bool _can_use_render_priority() const override { return true; }

public:
	void set_albedo(const Color &p_albedo);
	Color get_albedo() const;

	void set_specular(float p_specular);
	float get_specular() const;

	void set_metallic(float p_metallic);
	float get_metallic() const;

	void set_roughness(float p_roughness);
	float get_roughness() const;

	void set_emission(const Color &p_emission);
	Color get_emission() const;

	void set_emission_energy_multiplier(float p_emission_energy_multiplier);
	float get_emission_energy_multiplier() const;

	void set_emission_intensity(float p_emission_intensity);
	float get_emission_intensity() const;

	void set_normal_scale(float p_normal_scale);
	float get_normal_scale() const;

	void set_rim(float p_rim);
	float get_rim() const;

	void set_rim_tint(float p_rim_tint);
	float get_rim_tint() const;

	void set_ao_light_affect(float p_ao_light_affect);
	float get_ao_light_affect() const;

	void set_clearcoat(float p_clearcoat);
	float get_clearcoat() const;

	void set_clearcoat_roughness(float p_clearcoat_roughness);
	float get_clearcoat_roughness() const;

	void set_anisotropy(float p_anisotropy);
	float get_anisotropy() const;

	void set_heightmap_scale(float p_heightmap_scale);
	float get_heightmap_scale() const;

	void set_heightmap_deep_parallax(bool p_enable);
	bool is_heightmap_deep_parallax_enabled() const;

	void set_heightmap_deep_parallax_min_layers(int p_layer);
	int get_heightmap_deep_parallax_min_layers() const;

	void set_heightmap_deep_parallax_max_layers(int p_layer);
	int get_heightmap_deep_parallax_max_layers() const;

	void set_heightmap_deep_parallax_flip_tangent(bool p_flip);
	bool get_heightmap_deep_parallax_flip_tangent() const;

	void set_heightmap_deep_parallax_flip_binormal(bool p_flip);
	bool get_heightmap_deep_parallax_flip_binormal() const;

	void set_heightmap_self_shadow(bool p_enable);
	bool is_heightmap_self_shadow_enabled() const;
	static Color get_metal_albedo(Metal p_metal); // Mariner M8.
	void set_heightmap_self_shadow_strength(float p_strength);
	// Mariner M6, F5, F6, M4.
	void set_sheen_color(const Color &p_value);
	Color get_sheen_color() const;
	void set_sheen_roughness(float p_value);
	float get_sheen_roughness() const;

	// Mariner G4: how the material gets wet with Environment.wetness (WETNESS) outside cover (OVERHEAD_COVER).
	void set_wetness_amount(float p_value);
	float get_wetness_amount() const;
	void set_wetness_porosity(float p_value);
	float get_wetness_porosity() const;
	void set_wetness_puddles(float p_value);
	float get_wetness_puddles() const;

	// Mariner G4b: how snow lies on the material with Environment.snow (SNOW) outside cover (OVERHEAD_COVER).
	void set_snow_amount(float p_value);
	float get_snow_amount() const;
	void set_snow_color(const Color &p_color);
	Color get_snow_color() const;
	void set_snow_roughness(float p_value);
	float get_snow_roughness() const;
	void set_snow_max_slope(float p_degrees);
	float get_snow_max_slope() const;
	void set_snow_relief(float p_value);
	float get_snow_relief() const;

	// Mariner G5a: fake interiors behind the windows (interior mapping): rooms of an atlas seen through the glass.
	void set_interior_rooms(const Vector2 &p_value);
	Vector2 get_interior_rooms() const;
	void set_interior_room_size(const Vector3 &p_value);
	Vector3 get_interior_room_size() const;
	void set_interior_atlas_size(const Vector2i &p_value);
	Vector2i get_interior_atlas_size() const;
	void set_interior_back_wall(float p_value);
	float get_interior_back_wall() const;
	void set_interior_brightness(float p_value);
	float get_interior_brightness() const;
	void set_interior_lights(float p_value);
	float get_interior_lights() const;
	void set_interior_lit_fraction(float p_value);
	float get_interior_lit_fraction() const;
	void set_interior_curtains(float p_value); // Mariner G5d.
	float get_interior_curtains() const;
	void set_interior_blinds(float p_value);
	float get_interior_blinds() const;
	void set_interior_curtain_color(const Color &p_value);
	Color get_interior_curtain_color() const;
	void set_interior_light_color(const Color &p_value);
	Color get_interior_light_color() const;
	void set_iridescence(float p_value);
	float get_iridescence() const;
	void set_iridescence_ior(float p_value);
	float get_iridescence_ior() const;
	void set_iridescence_thickness_min(float p_value);
	float get_iridescence_thickness_min() const;
	void set_iridescence_thickness_max(float p_value);
	float get_iridescence_thickness_max() const;
	void set_refraction_ior(float p_value);
	float get_refraction_ior() const;
	void set_refraction_thickness(float p_value);
	float get_refraction_thickness() const;
	void set_refraction_absorption_color(const Color &p_value);
	Color get_refraction_absorption_color() const;
	void set_refraction_absorption_distance(float p_value);
	float get_refraction_absorption_distance() const;
	void set_refraction_physical(bool p_enable);
	bool is_refraction_physical() const;
	void set_detail_normal_rnm(bool p_enable);
	bool is_detail_normal_rnm() const;
	float get_heightmap_self_shadow_strength() const;
	void set_heightmap_depth_offset(bool p_enable);
	bool is_heightmap_depth_offset_enabled() const;
	// Mariner G5b: deep parallax of box impostor faces - the true view ray, clipped to the face's rectangle (CUSTOM0).
	void set_heightmap_impostor(bool p_enable);
	bool is_heightmap_impostor_enabled() const;
	// Mariner G5c: the box impostor's faces for the visual hull (BoxImpostorBaker): 4 vectors per face (+X, -X, +Z, -Z,
	// +Y, -Y: U, V and depth from object space, the face's tangent and sign or 0 if not baked), then the box's min and
	// max. With them the view ray is traced through all the faces' reliefs at once.
	static constexpr int IMPOSTOR_FACE_DATA_SIZE = 26;
	void set_heightmap_impostor_faces(const PackedVector4Array &p_faces);
	PackedVector4Array get_heightmap_impostor_faces() const;

	// Mariner G6: the planar reflection of a PlanarReflection3D (its texture is set by the node).
	void set_planar_reflection_amount(float p_value);
	float get_planar_reflection_amount() const;
	void set_planar_reflection_distortion(float p_value);
	float get_planar_reflection_distortion() const;

	void set_displacement_scale(float p_value); // Mariner G7.
	float get_displacement_scale() const;
	void set_displacement_midlevel(float p_value);
	float get_displacement_midlevel() const;
	void set_displacement_texture_lod(float p_value);
	float get_displacement_texture_lod() const;
	void set_displacement_normal(bool p_enable);
	bool is_displacement_normal_enabled() const;

	void set_subsurface_scattering_strength(float p_subsurface_scattering_strength);
	float get_subsurface_scattering_strength() const;

	void set_transmittance_color(const Color &p_color);
	Color get_transmittance_color() const;

	void set_transmittance_depth(float p_depth);
	float get_transmittance_depth() const;

	void set_transmittance_boost(float p_boost);
	float get_transmittance_boost() const;

	void set_backlight(const Color &p_backlight);
	Color get_backlight() const;

	void set_refraction(float p_refraction);
	float get_refraction() const;

	void set_point_size(float p_point_size);
	float get_point_size() const;

	void set_transparency(Transparency p_transparency);
	Transparency get_transparency() const;

	// Mariner AA2.
	void set_alpha_mask_mode(AlphaMaskMode p_mode);
	AlphaMaskMode get_alpha_mask_mode() const;
	static Ref<Image> bake_alpha_mask(const Ref<Image> &p_source, const Vector2i &p_size, AlphaMaskMode p_mode, float p_range = 4.0);

	void set_alpha_antialiasing(AlphaAntiAliasing p_alpha_aa);
	AlphaAntiAliasing get_alpha_antialiasing() const;

	void set_alpha_antialiasing_edge(float p_edge);
	float get_alpha_antialiasing_edge() const;

	void set_shading_mode(ShadingMode p_shading_mode);
	ShadingMode get_shading_mode() const;

	void set_detail_uv(DetailUV p_detail_uv);
	DetailUV get_detail_uv() const;

	void set_blend_mode(BlendMode p_mode);
	BlendMode get_blend_mode() const;

	void set_detail_blend_mode(BlendMode p_mode);
	BlendMode get_detail_blend_mode() const;

	void set_depth_draw_mode(DepthDrawMode p_mode);
	DepthDrawMode get_depth_draw_mode() const;

	void set_depth_test(DepthTest p_func);
	DepthTest get_depth_test() const;

	void set_cull_mode(CullMode p_mode);
	CullMode get_cull_mode() const;

	void set_diffuse_mode(DiffuseMode p_mode);
	DiffuseMode get_diffuse_mode() const;

	void set_specular_mode(SpecularMode p_mode);
	SpecularMode get_specular_mode() const;

	void set_flag(Flags p_flag, bool p_enabled);
	bool get_flag(Flags p_flag) const;

	void set_texture(TextureParam p_param, const Ref<Texture2D> &p_texture);
	Ref<Texture2D> get_texture(TextureParam p_param) const;
	// Used only for shader material conversion
	Ref<Texture2D> get_texture_by_name(const StringName &p_name) const;

	void set_texture_filter(TextureFilter p_filter);
	TextureFilter get_texture_filter() const;

	void set_feature(Feature p_feature, bool p_enabled);
	bool get_feature(Feature p_feature) const;

	void set_uv1_scale(const Vector3 &p_scale);
	Vector3 get_uv1_scale() const;

	void set_uv1_offset(const Vector3 &p_offset);
	Vector3 get_uv1_offset() const;

	void set_uv1_triplanar_blend_sharpness(float p_sharpness);
	float get_uv1_triplanar_blend_sharpness() const;

	void set_uv2_scale(const Vector3 &p_scale);
	Vector3 get_uv2_scale() const;

	void set_uv2_offset(const Vector3 &p_offset);
	Vector3 get_uv2_offset() const;

	void set_uv2_triplanar_blend_sharpness(float p_sharpness);
	float get_uv2_triplanar_blend_sharpness() const;

	void set_billboard_mode(BillboardMode p_mode);
	BillboardMode get_billboard_mode() const;

	void set_particles_anim_h_frames(int p_frames);
	int get_particles_anim_h_frames() const;
	void set_particles_anim_v_frames(int p_frames);
	int get_particles_anim_v_frames() const;

	void set_particles_anim_loop(bool p_loop);
	bool get_particles_anim_loop() const;

	void set_grow_enabled(bool p_enable);
	bool is_grow_enabled() const;

	// Mariner AA3: thin tubes (wires, cables, thin branches) along the local Y axis, at least a pixel wide.
	void set_wire_antialiasing(bool p_enable);
	bool is_wire_antialiasing() const;

	void set_grow(float p_grow);
	float get_grow() const;

	void set_alpha_scissor_threshold(float p_threshold);
	float get_alpha_scissor_threshold() const;

	void set_alpha_hash_scale(float p_scale);
	float get_alpha_hash_scale() const;

	void set_on_top_of_alpha();

	void set_proximity_fade_enabled(bool p_enable);
	bool is_proximity_fade_enabled() const;

	void set_proximity_fade_distance(float p_distance);
	float get_proximity_fade_distance() const;

	void set_msdf_pixel_range(float p_range);
	float get_msdf_pixel_range() const;

	void set_msdf_outline_size(float p_size);
	float get_msdf_outline_size() const;

	void set_distance_fade(DistanceFadeMode p_mode);
	DistanceFadeMode get_distance_fade() const;

	void set_distance_fade_max_distance(float p_distance);
	float get_distance_fade_max_distance() const;

	void set_distance_fade_min_distance(float p_distance);
	float get_distance_fade_min_distance() const;

	void set_emission_operator(EmissionOperator p_op);
	EmissionOperator get_emission_operator() const;

	void set_stencil_mode(StencilMode p_stencil_mode);
	StencilMode get_stencil_mode() const;

	void set_stencil_flags(int p_stencil_flags);
	int get_stencil_flags() const;

	void set_stencil_compare(StencilCompare p_op);
	StencilCompare get_stencil_compare() const;

	void set_stencil_reference(int p_reference);
	int get_stencil_reference() const;

	void set_stencil_effect_color(const Color &p_color);
	Color get_stencil_effect_color() const;

	void set_stencil_effect_outline_thickness(float p_outline_thickness);
	float get_stencil_effect_outline_thickness() const;

	void set_metallic_texture_channel(TextureChannel p_channel);
	TextureChannel get_metallic_texture_channel() const;
	void set_roughness_texture_channel(TextureChannel p_channel);
	TextureChannel get_roughness_texture_channel() const;
	void set_ao_texture_channel(TextureChannel p_channel);
	TextureChannel get_ao_texture_channel() const;
	void set_refraction_texture_channel(TextureChannel p_channel);
	TextureChannel get_refraction_texture_channel() const;

	void set_z_clip_scale(float p_z_clip_scale);
	float get_z_clip_scale() const;
	void set_fov_override(float p_fov_override);
	float get_fov_override() const;

	static void init_shaders();
	static void finish_shaders();
	static void flush_changes();

	static Ref<Material> get_material_for_2d(bool p_shaded, Transparency p_transparency, bool p_double_sided, bool p_billboard = false, bool p_billboard_y = false, bool p_msdf = false, bool p_no_depth = false, bool p_fixed_size = false, TextureFilter p_filter = TEXTURE_FILTER_LINEAR_WITH_MIPMAPS, AlphaAntiAliasing p_alpha_antialiasing_mode = ALPHA_ANTIALIASING_OFF, bool p_texture_repeat = false, RID *r_shader_rid = nullptr);

	virtual RID get_rid() const override;
	virtual RID get_shader_rid() const override;

	virtual Shader::Mode get_shader_mode() const override;

	BaseMaterial3D(bool p_orm);
	virtual ~BaseMaterial3D();
};

VARIANT_ENUM_CAST(BaseMaterial3D::TextureParam)
VARIANT_ENUM_CAST(BaseMaterial3D::TextureFilter)
VARIANT_ENUM_CAST(BaseMaterial3D::ShadingMode)
VARIANT_ENUM_CAST(BaseMaterial3D::Transparency)
VARIANT_ENUM_CAST(BaseMaterial3D::AlphaAntiAliasing)
VARIANT_ENUM_CAST(BaseMaterial3D::AlphaMaskMode)
VARIANT_ENUM_CAST(BaseMaterial3D::DetailUV)
VARIANT_ENUM_CAST(BaseMaterial3D::Feature)
VARIANT_ENUM_CAST(BaseMaterial3D::Metal)
VARIANT_ENUM_CAST(BaseMaterial3D::BlendMode)
VARIANT_ENUM_CAST(BaseMaterial3D::DepthDrawMode)
VARIANT_ENUM_CAST(BaseMaterial3D::DepthTest)
VARIANT_ENUM_CAST(BaseMaterial3D::CullMode)
VARIANT_ENUM_CAST(BaseMaterial3D::Flags)
VARIANT_ENUM_CAST(BaseMaterial3D::DiffuseMode)
VARIANT_ENUM_CAST(BaseMaterial3D::SpecularMode)
VARIANT_ENUM_CAST(BaseMaterial3D::BillboardMode)
VARIANT_ENUM_CAST(BaseMaterial3D::TextureChannel)
VARIANT_ENUM_CAST(BaseMaterial3D::EmissionOperator)
VARIANT_ENUM_CAST(BaseMaterial3D::DistanceFadeMode)
VARIANT_ENUM_CAST(BaseMaterial3D::StencilMode)
VARIANT_ENUM_CAST(BaseMaterial3D::StencilFlags)
VARIANT_ENUM_CAST(BaseMaterial3D::StencilCompare)

class StandardMaterial3D : public BaseMaterial3D {
	GDCLASS(StandardMaterial3D, BaseMaterial3D)
protected:
#ifndef DISABLE_DEPRECATED
	// Kept for compatibility from 3.x to 4.0.
	bool _set(const StringName &p_name, const Variant &p_value);
#endif

public:
	StandardMaterial3D() :
			BaseMaterial3D(false) {}
};

class ORMMaterial3D : public BaseMaterial3D {
	GDCLASS(ORMMaterial3D, BaseMaterial3D)
public:
	ORMMaterial3D() :
			BaseMaterial3D(true) {}
};

class PlaceholderMaterial : public Material {
	GDCLASS(PlaceholderMaterial, Material)
public:
	virtual RID get_shader_rid() const override { return RID(); }
	virtual Shader::Mode get_shader_mode() const override { return Shader::MODE_CANVAS_ITEM; }
};

//////////////////////
