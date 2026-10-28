// Mariner E5c: scene data of the per-viewport TLAS (patch E5a, RenderRaytracing) for the path tracer.
// Layouts must match RT_InstanceData / RT_MaterialData in render_raytracing.h (std430, 128 / 96 bytes).
// Vertex fetch functions are ported from NVIDIA's Godot fork (raytracing_hit_inc.glsl), adapted to the E5a layout
// (MultiMesh instance colors, layer mask) and to inline ray queries.
//
// Requires: GL_EXT_buffer_reference, GL_ARB_gpu_shader_int64, oct_inc.glsl, PT_SCENE_SET.

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer FloatBuffer {
	float v[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer Uint32Buffer {
	uint v[];
};

const uint OFFSET_NONE = 0xFFFFFFFFu;

const uint GEOM_FLAG_COMPRESSED = 1u;
const uint GEOM_FLAG_DEFORMED = 4u;
const uint GEOM_FLAG_MULTIMESH = 8u;
const uint GEOM_FLAG_MULTIMESH_INSTANCE = 16u;

const uint MAT_FLAG_HAS_NORMAL_MAP = 1u;
const uint MAT_FLAG_HAS_EMISSION_TEX = 2u;
const uint MAT_FLAG_POINT_FILTER = 4u;
const uint MAT_FLAG_USE_VERTEX_COLOR = 8u;
const uint MAT_FLAG_CUSTOM_SHADER = 16u;
const uint MAT_FLAG_UNSHADED = 32u;
// Dielectric medium (water): absorption in emission_color (1/m), scattering in uv1_scale.xy + uv1_offset.x (1/m),
// IOR in specular, interface roughness in roughness, foam color in albedo_color (coverage = vertex color alpha).
const uint MAT_FLAG_DIELECTRIC = 64u;
// Dielectric detail (render_raytracing.h): per-pixel wave slopes (normal / orm texture, scales in metallic,
// ao_strength, normal_map_depth, emission_strength; mesh UV = undisplaced object x, z; vertex color RG = extra slope,
// B = gain of the wave maps) and a foam threshold pattern (albedo texture, half2 scales in the bits of uv1_offset.y).
const uint MAT_FLAG_WAVE_DETAIL = 128u;
const uint MAT_FLAG_FOAM_PATTERN = 256u;

struct GeometryData {
	uint64_t vertex_address;
	uint64_t attribute_address;
	uint64_t index_address;
	uint64_t multimesh_address;
	uint vertex_count;
	uint position_stride;
	uint normal_byte_offset;
	uint normal_stride;
	uint tangent_byte_offset;
	uint tangent_stride;
	uint attribute_stride;
	uint uv_byte_offset;
	uint uv_scale_packed;
	uint index_format; // 0: uint16, 1: uint32, 2: none.
	uint primitive_count;
	uint flags;
	float aabb_size_x;
	float aabb_size_y;
	float aabb_size_z;
	uint color_byte_offset;
	float aabb_pos_x;
	float aabb_pos_y;
	float aabb_pos_z;
	uint layer_mask;
	uint mm_vertices_per_instance;
	uint mm_stride;
	uint mm_color_offset;
	uint mm_custom_offset;
};

struct MaterialData {
	uint albedo_texture_idx;
	uint normal_texture_idx;
	uint orm_texture_idx;
	uint emission_texture_idx;
	vec4 albedo_color;
	vec3 emission_color;
	float emission_strength;
	float metallic;
	float roughness;
	float ao_strength;
	uint flags;
	vec2 uv1_scale;
	vec2 uv1_offset;
	float normal_map_depth;
	float specular;
	float alpha_scissor;
	uint cull_mode;
};

layout(set = PT_SCENE_SET, binding = 0) uniform accelerationStructureEXT tlas;

layout(set = PT_SCENE_SET, binding = 1, std430) restrict readonly buffer GeometryBuffer {
	GeometryData geometries[];
};

layout(set = PT_SCENE_SET, binding = 2, std430) restrict readonly buffer MaterialBuffer {
	MaterialData materials[];
};

// ============================================================================
// Vertex attributes (fork: raytracing_hit_inc.glsl)
// ============================================================================

struct TBNResult {
	vec3 normal;
	vec3 tangent;
	vec3 binormal;
	float bitangent_sign;
};

void get_triangle_indices_ex(GeometryData geom, uint primitive_id, out uint i0, out uint i1, out uint i2) {
	if (geom.index_format == 2u || geom.index_address == 0ul) {
		i0 = primitive_id * 3u;
		i1 = primitive_id * 3u + 1u;
		i2 = primitive_id * 3u + 2u;
	} else {
		Uint32Buffer idx = Uint32Buffer(geom.index_address);
		if (geom.index_format == 0u) {
			uint byte_off = primitive_id * 6u;
			uint word0 = idx.v[byte_off >> 2];
			uint word1 = idx.v[(byte_off >> 2) + 1u];
			if ((byte_off & 3u) == 0u) {
				i0 = word0 & 0xFFFFu;
				i1 = word0 >> 16;
				i2 = word1 & 0xFFFFu;
			} else {
				i0 = word0 >> 16;
				i1 = word1 & 0xFFFFu;
				i2 = word1 >> 16;
			}
		} else {
			i0 = idx.v[primitive_id * 3u];
			i1 = idx.v[primitive_id * 3u + 1u];
			i2 = idx.v[primitive_id * 3u + 2u];
		}
	}
}

vec2 fetch_uv(GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	if (geom.uv_byte_offset == OFFSET_NONE || geom.attribute_address == 0ul) {
		return vec2(0.0);
	}
	uint stride_words = geom.attribute_stride >> 2;
	uint uv_word_off = geom.uv_byte_offset >> 2;
	vec2 uv0, uv1, uv2;
	if ((geom.flags & GEOM_FLAG_COMPRESSED) != 0u) {
		Uint32Buffer attr = Uint32Buffer(geom.attribute_address);
		uint p0 = attr.v[i0 * stride_words + uv_word_off];
		uint p1 = attr.v[i1 * stride_words + uv_word_off];
		uint p2 = attr.v[i2 * stride_words + uv_word_off];
		uv0 = vec2(float(p0 & 0xFFFFu), float(p0 >> 16)) / 65535.0;
		uv1 = vec2(float(p1 & 0xFFFFu), float(p1 >> 16)) / 65535.0;
		uv2 = vec2(float(p2 & 0xFFFFu), float(p2 >> 16)) / 65535.0;
		vec2 scale = unpackHalf2x16(geom.uv_scale_packed);
		if (scale.x != 0.0 || scale.y != 0.0) {
			uv0 = (uv0 - 0.5) * scale;
			uv1 = (uv1 - 0.5) * scale;
			uv2 = (uv2 - 0.5) * scale;
		}
	} else {
		FloatBuffer attr_f = FloatBuffer(geom.attribute_address);
		uv0 = vec2(attr_f.v[i0 * stride_words + uv_word_off], attr_f.v[i0 * stride_words + uv_word_off + 1u]);
		uv1 = vec2(attr_f.v[i1 * stride_words + uv_word_off], attr_f.v[i1 * stride_words + uv_word_off + 1u]);
		uv2 = vec2(attr_f.v[i2 * stride_words + uv_word_off], attr_f.v[i2 * stride_words + uv_word_off + 1u]);
	}
	return bary.x * uv0 + bary.y * uv1 + bary.z * uv2;
}

vec4 fetch_color(GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	vec4 color = vec4(1.0);
	if (geom.color_byte_offset != OFFSET_NONE && geom.attribute_address != 0ul) {
		Uint32Buffer attr = Uint32Buffer(geom.attribute_address);
		uint stride_words = geom.attribute_stride >> 2;
		uint off = geom.color_byte_offset >> 2;
		color = bary.x * unpackUnorm4x8(attr.v[i0 * stride_words + off]) + bary.y * unpackUnorm4x8(attr.v[i1 * stride_words + off]) + bary.z * unpackUnorm4x8(attr.v[i2 * stride_words + off]);
	}
	// MultiMesh instance color (Godot multiplies COLOR by it).
	if (geom.multimesh_address != 0ul && geom.mm_color_offset != OFFSET_NONE) {
		uint inst = geom.mm_vertices_per_instance == 0u ? 0u : i0 / geom.mm_vertices_per_instance;
		FloatBuffer mb = FloatBuffer(geom.multimesh_address);
		uint o = inst * geom.mm_stride + geom.mm_color_offset;
		color *= vec4(mb.v[o], mb.v[o + 1u], mb.v[o + 2u], mb.v[o + 3u]);
	}
	return color;
}

void axis_angle_to_tbn(vec3 axis, float angle, out vec3 tangent, out vec3 binormal, out vec3 normal) {
	float c = cos(angle);
	float s = sin(angle);
	vec3 omc_axis = (1.0 - c) * axis;
	vec3 s_axis = s * axis;
	tangent = omc_axis.xxx * axis + vec3(c, -s_axis.z, s_axis.y);
	binormal = omc_axis.yyy * axis + vec3(s_axis.z, c, -s_axis.x);
	normal = omc_axis.zzz * axis + vec3(-s_axis.y, s_axis.x, c);
}

TBNResult fetch_tbn_compressed_vertex(GeometryData geom, uint idx) {
	TBNResult result;
	Uint32Buffer vb = Uint32Buffer(geom.vertex_address);
	uint n_packed = vb.v[(geom.normal_byte_offset >> 2) + idx * (geom.normal_stride >> 2)];
	vec2 axis_oct = vec2(float(n_packed & 0xFFFFu), float(n_packed >> 16)) / 65535.0 * 2.0 - 1.0;
	vec3 axis = oct_to_vec3(axis_oct);
	uint pos_word1 = vb.v[idx * 2u + 1u];
	float angle_raw = float(pos_word1 >> 16) / 65535.0;
	result.bitangent_sign = angle_raw > 0.5 ? 1.0 : -1.0;
	float angle = abs(angle_raw * 2.0 - 1.0) * PI;
	axis_angle_to_tbn(axis, angle, result.tangent, result.binormal, result.normal);
	result.binormal *= result.bitangent_sign;
	return result;
}

TBNResult fetch_tbn_uncompressed_vertex(GeometryData geom, uint idx) {
	TBNResult result;
	Uint32Buffer vb = Uint32Buffer(geom.vertex_address);
	uint norm_base = geom.normal_byte_offset >> 2;
	uint norm_stride = geom.normal_stride >> 2;
	uint n_packed = vb.v[norm_base + idx * norm_stride];
	vec2 n_oct = vec2(float(n_packed & 0xFFFFu), float(n_packed >> 16)) / 65535.0 * 2.0 - 1.0;
	result.normal = oct_to_vec3(n_oct);
	if (geom.normal_stride >= 8u) {
		uint t_packed = vb.v[norm_base + idx * norm_stride + 1u];
		vec2 t_raw = vec2(float(t_packed & 0xFFFFu), float(t_packed >> 16)) / 65535.0 * 2.0 - 1.0;
		vec2 t_oct = vec2(t_raw.x, abs(t_raw.y) * 2.0 - 1.0);
		result.tangent = oct_to_vec3(t_oct);
		result.bitangent_sign = t_raw.y < 0.0 ? -1.0 : 1.0;
		result.binormal = normalize(cross(result.normal, result.tangent) * result.bitangent_sign);
	} else {
		result.tangent = vec3(1.0, 0.0, 0.0);
		result.binormal = vec3(0.0, 0.0, 1.0);
		result.bitangent_sign = 1.0;
	}
	return result;
}

TBNResult fetch_tbn(GeometryData geom, uint i0, uint i1, uint i2, vec3 bary) {
	TBNResult result;
	result.normal = vec3(0.0, 1.0, 0.0);
	result.tangent = vec3(1.0, 0.0, 0.0);
	result.binormal = vec3(0.0, 0.0, 1.0);
	result.bitangent_sign = 1.0;
	if (geom.normal_byte_offset == OFFSET_NONE || geom.vertex_address == 0ul) {
		return result;
	}
	TBNResult t0, t1, t2;
	if ((geom.flags & GEOM_FLAG_COMPRESSED) != 0u) {
		t0 = fetch_tbn_compressed_vertex(geom, i0);
		t1 = fetch_tbn_compressed_vertex(geom, i1);
		t2 = fetch_tbn_compressed_vertex(geom, i2);
	} else {
		t0 = fetch_tbn_uncompressed_vertex(geom, i0);
		t1 = fetch_tbn_uncompressed_vertex(geom, i1);
		t2 = fetch_tbn_uncompressed_vertex(geom, i2);
	}
	result.normal = normalize(bary.x * t0.normal + bary.y * t1.normal + bary.z * t2.normal);
	result.tangent = normalize(bary.x * t0.tangent + bary.y * t1.tangent + bary.z * t2.tangent);
	result.binormal = normalize(bary.x * t0.binormal + bary.y * t1.binormal + bary.z * t2.binormal);
	result.bitangent_sign = (bary.x * t0.bitangent_sign + bary.y * t1.bitangent_sign + bary.z * t2.bitangent_sign) < 0.0 ? -1.0 : 1.0;
	return result;
}

// Object space position of a vertex in BLAS space (compressed meshes: [0, 1] of the AABB, as in the TLAS).
vec3 fetch_position(GeometryData geom, uint i) {
	if ((geom.flags & GEOM_FLAG_COMPRESSED) != 0u) {
		Uint32Buffer vb = Uint32Buffer(geom.vertex_address);
		uint w0 = vb.v[i * 2u];
		uint w1 = vb.v[i * 2u + 1u];
		return vec3(float(w0 & 0xFFFFu), float(w0 >> 16), float(w1 & 0xFFFFu)) / 65535.0;
	}
	FloatBuffer vb = FloatBuffer(geom.vertex_address);
	uint s = geom.position_stride >> 2;
	return vec3(vb.v[i * s], vb.v[i * s + 1u], vb.v[i * s + 2u]);
}

// ============================================================================
// Hit data from an inline ray query
// ============================================================================

struct RayHit {
	bool hit;
	float t;
	uint instance;
	uint primitive;
	bool front_face;
	vec2 barycentrics;
	mat4x3 object_to_world;
	mat4x3 world_to_object;
};

struct HitData {
	vec3 hit_pos;
	vec3 face_normal; // World, true triangle normal (not flipped).
	vec3 geometry_normal; // World, flipped toward the ray.
	vec3 shading_normal; // World, interpolated vertex normal (flipped like geometry_normal).
	vec3 vertex_normal; // World, interpolated vertex normal, not flipped (outside of a dielectric).
	vec3 tangent;
	vec3 bitangent;
	vec2 uv; // Raw UV (no material scale/offset).
	vec4 color; // Vertex color x MultiMesh instance color.
	bool is_front_face;
	uint geometry_idx;
};

HitData compute_hit_data(RayHit r, vec3 ray_origin, vec3 ray_dir) {
	HitData h;
	h.geometry_idx = r.instance;
	h.is_front_face = r.front_face;
	GeometryData geom = geometries[r.instance];
	vec3 bary = vec3(1.0 - r.barycentrics.x - r.barycentrics.y, r.barycentrics.x, r.barycentrics.y);
	uint i0, i1, i2;
	get_triangle_indices_ex(geom, r.primitive, i0, i1, i2);

	h.hit_pos = ray_origin + ray_dir * r.t;

	vec3 p0 = r.object_to_world * vec4(fetch_position(geom, i0), 1.0);
	vec3 p1 = r.object_to_world * vec4(fetch_position(geom, i1), 1.0);
	vec3 p2 = r.object_to_world * vec4(fetch_position(geom, i2), 1.0);
	vec3 fn = cross(p1 - p0, p2 - p0);
	float fl = length(fn);
	h.face_normal = fl > 0.0 ? fn / fl : -ray_dir;
	h.geometry_normal = dot(h.face_normal, ray_dir) > 0.0 ? -h.face_normal : h.face_normal;

	h.uv = fetch_uv(geom, i0, i1, i2, bary);
	h.color = fetch_color(geom, i0, i1, i2, bary);

	h.vertex_normal = h.face_normal;
	h.tangent = vec3(1.0, 0.0, 0.0);
	h.bitangent = vec3(0.0, 0.0, 1.0);
	if (geom.normal_byte_offset != OFFSET_NONE) {
		TBNResult tbn = fetch_tbn(geom, i0, i1, i2, bary);
		vec3 n = tbn.normal;
		vec3 t = tbn.tangent;
		if ((geom.flags & GEOM_FLAG_COMPRESSED) != 0u) {
			// BLAS space is the [0, 1] AABB box: normals scale with the box size, tangents with its inverse.
			vec3 size = vec3(geom.aabb_size_x, geom.aabb_size_y, geom.aabb_size_z);
			n *= size;
			t /= max(size, vec3(1e-6));
		}
		// Normals: inverse transpose of object -> world = rows of world -> object.
		n = normalize(n * mat3(r.world_to_object));
		t = normalize(mat3(r.object_to_world) * t);
		t = normalize(t - n * dot(n, t));
		h.vertex_normal = n;
		h.tangent = t;
		h.bitangent = cross(n, t) * tbn.bitangent_sign;
	}
	h.shading_normal = dot(h.vertex_normal, h.geometry_normal) < 0.0 ? -h.vertex_normal : h.vertex_normal;
	if (dot(h.shading_normal, h.geometry_normal) < 0.0) {
		h.shading_normal = h.geometry_normal;
	}
	return h;
}

vec3 apply_normal_map(HitData h, vec3 tangent_space_normal, float normal_map_depth) {
	vec3 n = h.shading_normal;
	vec3 t = h.tangent;
	vec3 b = h.bitangent;
	if (dot(h.vertex_normal, h.shading_normal) < 0.0) {
		b = -b; // Back face: mirror the frame with the normal.
	}
	vec3 mapped = t * tangent_space_normal.x + b * tangent_space_normal.y + n * tangent_space_normal.z;
	return normalize(mix(n, mapped, normal_map_depth));
}
