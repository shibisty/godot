/**************************************************************************/
/*  test_mariner_e17.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_e17)

#include "core/config/project_settings.h"
#include "core/io/file_access.h"
#include "core/io/image.h"
#include "scene/resources/compressed_texture.h"
#include "scene/resources/mesh.h"
#include "scene/resources/texture_streamer.h"
#include "servers/rendering/renderer_texture_streaming.h"
#include "servers/rendering/rendering_server.h"
#include "tests/test_utils.h"

// Mariner engine patch E17: texture mip streaming. Here: the UV density of meshes, the demand and mip math, the
// budget fit, partial reads of .ctex files and the streaming of a CompressedTexture2D driven by injected demand;
// the image and the memory on lavapipe by engine/tests/e17/streaming.gd.

namespace TestMarinerE17 {

static Array quad_arrays(float p_size, float p_uv) {
	Array arrays;
	arrays.resize(Mesh::ARRAY_MAX);
	PackedVector3Array vertices = { Vector3(0, 0, 0), Vector3(p_size, 0, 0), Vector3(p_size, 0, p_size), Vector3(0, 0, p_size) };
	PackedVector2Array uvs = { Vector2(0, 0), Vector2(p_uv, 0), Vector2(p_uv, p_uv), Vector2(0, p_uv) };
	PackedInt32Array indices = { 0, 1, 2, 0, 2, 3 };
	PackedVector3Array normals = { Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0), Vector3(0, 1, 0) };
	arrays[Mesh::ARRAY_VERTEX] = vertices;
	arrays[Mesh::ARRAY_NORMAL] = normals;
	arrays[Mesh::ARRAY_TEX_UV] = uvs;
	arrays[Mesh::ARRAY_INDEX] = indices;
	return arrays;
}

// Every level is filled with its own grey, so a read can be checked by the colour of the level it starts at.
static Ref<Image> level_image(int p_size, Image::Format p_format) {
	Ref<Image> image = Image::create_empty(p_size, p_size, true, Image::FORMAT_RGBA8);
	for (int level = 0; level < image->get_mipmap_count() + 1; level++) {
		int w = 0;
		int h = 0;
		const int64_t offset = Image::get_image_mipmap_offset_and_dimensions(p_size, p_size, Image::FORMAT_RGBA8, level, w, h);
		uint8_t *data = image->ptrw() + offset;
		for (int i = 0; i < w * h; i++) {
			data[i * 4 + 0] = uint8_t(level * 20);
			data[i * 4 + 1] = uint8_t(level * 20);
			data[i * 4 + 2] = uint8_t(level * 20);
			data[i * 4 + 3] = 255;
		}
	}
	if (p_format != Image::FORMAT_RGBA8) {
		// Block data made up byte by byte: the reads only split the chain, and a real compressor could start a GPU device.
		Vector<uint8_t> blocks;
		blocks.resize(Image::get_image_data_size(p_size, p_size, p_format, true));
		for (int i = 0; i < blocks.size(); i++) {
			blocks.write[i] = uint8_t((i * 37) ^ (i >> 7));
		}
		return Image::create_from_data(p_size, p_size, true, p_format, blocks);
	}
	return image;
}

static void write_ctex(const String &p_path, const Ref<Image> &p_image) {
	Ref<FileAccess> f = FileAccess::open(p_path, FileAccess::WRITE);
	REQUIRE(f.is_valid());
	f->store_buffer((const uint8_t *)"GST2", 4);
	f->store_32(CompressedTexture2D::FORMAT_VERSION);
	f->store_32(p_image->get_width());
	f->store_32(p_image->get_height());
	f->store_32(CompressedTexture2D::FORMAT_BIT_HAS_MIPMAPS);
	f->store_32(0);
	f->store_32(0);
	f->store_32(0);
	f->store_32(0);
	f->store_32(CompressedTexture2D::DATA_FORMAT_IMAGE);
	f->store_16(p_image->get_width());
	f->store_16(p_image->get_height());
	f->store_32(p_image->get_mipmap_count());
	f->store_32(p_image->get_format());
	f->store_buffer(p_image->get_data());
}

struct StreamingSettings {
	Variant enabled;
	Variant resident;
	Variant min_size;

	StreamingSettings(int p_resident, int p_min_size) {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		enabled = ps->get_setting("rendering/textures/streaming/enabled");
		resident = ps->get_setting("rendering/textures/streaming/resident_size");
		min_size = ps->get_setting("rendering/textures/streaming/min_texture_size");
		ps->set_setting("rendering/textures/streaming/enabled", true);
		ps->set_setting("rendering/textures/streaming/resident_size", p_resident);
		ps->set_setting("rendering/textures/streaming/min_texture_size", p_min_size);
	}
	~StreamingSettings() {
		ProjectSettings *ps = ProjectSettings::get_singleton();
		ps->set_setting("rendering/textures/streaming/enabled", enabled);
		ps->set_setting("rendering/textures/streaming/resident_size", resident);
		ps->set_setting("rendering/textures/streaming/min_texture_size", min_size);
	}
};

static void inject_demand(RID p_texture, float p_demand, uint64_t p_frame) {
	RendererTextureStreaming *feedback = RendererTextureStreaming::get_singleton();
	REQUIRE(feedback != nullptr);
	feedback->begin_view();
	// Any RID stands for the material; the texture lookup below hands back the streamed texture for it.
	feedback->add_material(p_texture, p_demand);
	feedback->end_view(p_frame, [](RID p_material, LocalVector<RID> &r_textures, float &r_scale) {
		r_textures.push_back(p_material);
	});
}

static void advance_frame(uint64_t p_frame) {
	RendererTextureStreaming *feedback = RendererTextureStreaming::get_singleton();
	// A view with nothing in it still advances the frame the textures age against.
	feedback->begin_view();
	feedback->end_view(p_frame, [](RID p_material, LocalVector<RID> &r_textures, float &r_scale) {});
}

TEST_CASE("[Mariner][E17] UV density of mesh surfaces") {
	CHECK(RS::mesh_compute_uv_density(quad_arrays(2.0, 1.0)[Mesh::ARRAY_VERTEX], quad_arrays(2.0, 1.0)[Mesh::ARRAY_TEX_UV], quad_arrays(2.0, 1.0)[Mesh::ARRAY_INDEX]) == doctest::Approx(2.0));
	CHECK(RS::mesh_compute_uv_density(quad_arrays(2.0, 4.0)[Mesh::ARRAY_VERTEX], quad_arrays(2.0, 4.0)[Mesh::ARRAY_TEX_UV], quad_arrays(2.0, 4.0)[Mesh::ARRAY_INDEX]) == doctest::Approx(0.5));
	// Lines carry no area; a surface without UV area has no density.
	CHECK(RS::mesh_compute_uv_density(quad_arrays(2.0, 1.0)[Mesh::ARRAY_VERTEX], quad_arrays(2.0, 1.0)[Mesh::ARRAY_TEX_UV], quad_arrays(2.0, 1.0)[Mesh::ARRAY_INDEX], RSE::PRIMITIVE_LINES) == 0.0f);
	CHECK(RS::mesh_compute_uv_density(quad_arrays(2.0, 0.0)[Mesh::ARRAY_VERTEX], quad_arrays(2.0, 0.0)[Mesh::ARRAY_TEX_UV], quad_arrays(2.0, 0.0)[Mesh::ARRAY_INDEX]) == 0.0f);

	Ref<ArrayMesh> mesh;
	mesh.instantiate();
	mesh->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, quad_arrays(3.0, 1.0));
	CHECK(RS::get_singleton()->mesh_get_surface(mesh->get_rid(), 0).uv_density == doctest::Approx(3.0));

	// The density is saved with the mesh and read back.
	Array surfaces = mesh->call("_get_surfaces");
	Dictionary surface = surfaces[0];
	CHECK(float(surface["uv_density"]) == doctest::Approx(3.0));
	surface["uv_density"] = 7.0f;
	Ref<ArrayMesh> copy;
	copy.instantiate();
	copy->call("_set_surfaces", surfaces);
	CHECK(RS::get_singleton()->mesh_get_surface(copy->get_rid(), 0).uv_density == doctest::Approx(7.0));

	// Meshes saved without it get it measured from the packed (here compressed) vertex data when streaming is on.
	// (Compressed positions decode only together with normals, as imported meshes have them.)
	Ref<ArrayMesh> packed;
	packed.instantiate();
	packed->add_surface_from_arrays(Mesh::PRIMITIVE_TRIANGLES, quad_arrays(5.0, 2.0), TypedArray<Array>(), Dictionary(), Mesh::ARRAY_FLAG_COMPRESS_ATTRIBUTES);
	Array legacy = packed->call("_get_surfaces");
	Dictionary legacy_surface = legacy[0];
	legacy_surface.erase("uv_density");
	{
		StreamingSettings settings(64, 256);
		Ref<ArrayMesh> loaded;
		loaded.instantiate();
		loaded->call("_set_surfaces", legacy);
		CHECK(RS::get_singleton()->mesh_get_surface(loaded->get_rid(), 0).uv_density == doctest::Approx(2.5).epsilon(0.01));
	}
	Ref<ArrayMesh> off;
	off.instantiate();
	off->call("_set_surfaces", legacy);
	CHECK(RS::get_singleton()->mesh_get_surface(off->get_rid(), 0).uv_density == 0.0f);
}

TEST_CASE("[Mariner][E17] Demand and mip math") {
	Projection perspective;
	perspective.set_perspective(90.0, 1.0, 0.1, 100.0);
	CHECK(RendererTextureStreaming::pixels_per_unit(perspective, 1000.0f, 10.0f) == doctest::Approx(50.0));
	CHECK(RendererTextureStreaming::pixels_per_unit(perspective, 1000.0f, 20.0f) == doctest::Approx(25.0));
	Projection ortho;
	ortho.set_orthogonal(20.0, 1.0, 0.1, 100.0, false);
	CHECK(RendererTextureStreaming::pixels_per_unit(ortho, 1000.0f, 5.0f) == doctest::Approx(50.0));
	CHECK(RendererTextureStreaming::pixels_per_unit(ortho, 1000.0f, 50.0f) == doctest::Approx(50.0));

	const AABB box(Vector3(0, 0, 0), Vector3(2, 2, 2));
	CHECK(RendererTextureStreaming::distance_to_aabb(Vector3(1, 1, 1), box) == 0.0f);
	CHECK(RendererTextureStreaming::distance_to_aabb(Vector3(5, 1, 1), box) == doctest::Approx(3.0));
	CHECK(RendererTextureStreaming::distance_to_aabb(Vector3(5, 6, 1), box) == doctest::Approx(5.0));

	CHECK(TextureStreamer::wanted_mip(2048.0f, 2048, 12, 0.0f) == 0);
	CHECK(TextureStreamer::wanted_mip(3000.0f, 2048, 12, 0.0f) == 0);
	CHECK(TextureStreamer::wanted_mip(1024.0f, 2048, 12, 0.0f) == 1);
	CHECK(TextureStreamer::wanted_mip(1000.0f, 2048, 12, 0.0f) == 1);
	CHECK(TextureStreamer::wanted_mip(16.0f, 2048, 12, 0.0f) == 7);
	CHECK(TextureStreamer::wanted_mip(16.0f, 2048, 12, 1.0f) == 8);
	CHECK(TextureStreamer::wanted_mip(0.001f, 2048, 12, 0.0f) == 11);
	CHECK(TextureStreamer::wanted_mip(0.0f, 2048, 12, 0.0f) == 11);
}

TEST_CASE("[Mariner][E17] Memory budget") {
	CHECK(TextureStreamer::chain_bytes(256, 256, Image::FORMAT_RGBA8, 0) == uint64_t(Image::get_image_data_size(256, 256, Image::FORMAT_RGBA8, true)));
	CHECK(TextureStreamer::chain_bytes(256, 256, Image::FORMAT_RGBA8, 8) == 4u);
	CHECK(TextureStreamer::chain_bytes(256, 256, Image::FORMAT_RGBA8, 1) == uint64_t(Image::get_image_data_size(128, 128, Image::FORMAT_RGBA8, true)));

	LocalVector<LocalVector<uint64_t>> chains;
	LocalVector<const uint64_t *> chain_ptrs;
	LocalVector<int> wanted = { 0, 1, 2 };
	LocalVector<int> lowest = { 3, 3, 3 };
	chains.resize(3);
	for (uint32_t i = 0; i < 3; i++) {
		for (int m = 0; m < 9; m++) {
			chains[i].push_back(TextureStreamer::chain_bytes(256, 256, Image::FORMAT_RGBA8, m));
		}
		chain_ptrs.push_back(chains[i].ptr());
	}
	LocalVector<int> target;
	const uint64_t full = chains[0][0] + chains[1][1] + chains[2][2];
	CHECK(TextureStreamer::fit_budget(wanted, lowest, chain_ptrs, full, target) == 0);
	CHECK(target[0] == 0);
	CHECK(target[1] == 1);
	CHECK(target[2] == 2);
	// One byte short: every texture drops one level.
	CHECK(TextureStreamer::fit_budget(wanted, lowest, chain_ptrs, full - 1, target) == 1);
	CHECK(target[0] == 1);
	CHECK(target[1] == 2);
	CHECK(target[2] == 3);
	// Nothing fits: all stay at their lowest resident level.
	CHECK(TextureStreamer::fit_budget(wanted, lowest, chain_ptrs, 1, target) == 3);
	CHECK(target[0] == 3);
	CHECK(target[1] == 3);
	CHECK(target[2] == 3);
}

TEST_CASE("[Mariner][E17] Partial reads of .ctex files") {
	const String path = TestUtils::get_temp_path("mariner_e17_rgba.ctex");
	Ref<Image> source = level_image(256, Image::FORMAT_RGBA8);
	write_ctex(path, source);

	TextureStreamer::Info info;
	REQUIRE(TextureStreamer::probe(path, info) == OK);
	CHECK(info.width == 256);
	CHECK(info.mip_count == 9);
	CHECK(info.format == Image::FORMAT_RGBA8);

	Ref<Image> from2 = TextureStreamer::load_mips(path, 2);
	REQUIRE(from2.is_valid());
	CHECK(from2->get_width() == 64);
	CHECK(from2->has_mipmaps());
	CHECK(from2->get_mipmap_count() == 6);
	CHECK(from2->get_pixel(10, 10).r == doctest::Approx(40.0 / 255.0));
	CHECK(from2->get_data() == source->get_data().slice(Image::get_image_mipmap_offset(256, 256, Image::FORMAT_RGBA8, 2)));
	Ref<Image> last = TextureStreamer::load_mips(path, 8);
	CHECK(last->get_width() == 1);
	CHECK_FALSE(last->has_mipmaps());

	// Block-compressed chains split at a level the same way.
	Ref<Image> bc = level_image(256, Image::FORMAT_DXT1);
	REQUIRE(bc->is_compressed());
	{
		const String bc_path = TestUtils::get_temp_path("mariner_e17_bc.ctex");
		write_ctex(bc_path, bc);
		for (int level = 0; level < 9; level++) {
			Ref<Image> part = TextureStreamer::load_mips(bc_path, level);
			REQUIRE(part.is_valid());
			CHECK(part->get_format() == bc->get_format());
			CHECK(part->get_width() == (256 >> level));
			CHECK(part->get_data() == bc->get_data().slice(Image::get_image_mipmap_offset(256, 256, bc->get_format(), level)));
		}
	}

	// Lossless (PNG / WebP) files do not stream.
	Ref<FileAccess> f = FileAccess::open(path, FileAccess::READ_WRITE);
	f->seek(36);
	f->store_32(CompressedTexture2D::DATA_FORMAT_PNG);
	f.unref();
	CHECK(TextureStreamer::probe(path, info) == ERR_UNAVAILABLE);
}

TEST_CASE("[Mariner][E17] Streaming a CompressedTexture2D") {
	const String path = TestUtils::get_temp_path("mariner_e17_stream.ctex");
	write_ctex(path, level_image(512, Image::FORMAT_RGBA8));
	StreamingSettings settings(64, 256);

	Ref<CompressedTexture2D> texture;
	texture.instantiate();
	REQUIRE(texture->load(path) == OK);
	// 512 px keeps the 64 px level and below: level 3.
	CHECK(texture->get_streaming_mip() == 3);
	CHECK(texture->get_width() == 512);
	CHECK(texture->get_image()->get_width() == 512);
	CHECK(TextureStreamer::get_texture_count() == 1);
	CHECK(TextureStreamer::get_resident_bytes() == TextureStreamer::chain_bytes(512, 512, Image::FORMAT_RGBA8, 3));

	TextureStreamer::Settings s;
	s.drop_delay = 1.0f;
	// Unseen textures keep their lowest level.
	TextureStreamer::update(0.1, s, true);
	CHECK(texture->get_streaming_mip() == 3);

	// 128 screen pixels per UV unit on a 512 px texture: level 2.
	const RID rid = texture->get_rid();
	inject_demand(rid, 128.0f, 10);
	TextureStreamer::update(0.1, s, true);
	CHECK(texture->get_streaming_mip() == 2);
	inject_demand(rid, 600.0f, 11);
	TextureStreamer::update(0.1, s, true);
	CHECK(texture->get_streaming_mip() == 0);
	CHECK(TextureStreamer::get_resident_bytes() == TextureStreamer::chain_bytes(512, 512, Image::FORMAT_RGBA8, 0));

	// A budget below the full chain coarsens the texture until it fits.
	TextureStreamer::Settings tight = s;
	tight.budget_bytes = TextureStreamer::chain_bytes(512, 512, Image::FORMAT_RGBA8, 1);
	inject_demand(rid, 600.0f, 12);
	TextureStreamer::update(0.1, tight, true); // Over budget: the drop starts at once.
	TextureStreamer::update(0.1, tight, true);
	CHECK(texture->get_streaming_mip() == 1);

	// Out of view: the detail stays for the drop delay, then the texture returns to its lowest level.
	advance_frame(20);
	TextureStreamer::update(0.4, tight, true);
	CHECK(texture->get_streaming_mip() == 1);
	advance_frame(21);
	TextureStreamer::update(0.4, tight, true);
	CHECK(texture->get_streaming_mip() == 1);
	advance_frame(22);
	TextureStreamer::update(1.5, tight, true);
	CHECK(texture->get_streaming_mip() == 3);

	texture.unref();
	CHECK(TextureStreamer::get_texture_count() == 0);
}

TEST_CASE("[Mariner][E17] Small, unmipped and disabled textures load whole") {
	const String path = TestUtils::get_temp_path("mariner_e17_small.ctex");
	write_ctex(path, level_image(128, Image::FORMAT_RGBA8));
	Ref<CompressedTexture2D> texture;
	{
		StreamingSettings settings(64, 256);
		texture.instantiate();
		REQUIRE(texture->load(path) == OK);
		CHECK(texture->get_streaming_mip() == -1);
	}
	const String big = TestUtils::get_temp_path("mariner_e17_big.ctex");
	write_ctex(big, level_image(512, Image::FORMAT_RGBA8));
	texture.instantiate();
	REQUIRE(texture->load(big) == OK);
	CHECK(texture->get_streaming_mip() == -1); // Streaming is off by default.
}

} // namespace TestMarinerE17
