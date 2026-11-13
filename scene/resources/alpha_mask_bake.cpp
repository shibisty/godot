/**************************************************************************/
/*  alpha_mask_bake.cpp                                                   */
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

#include "alpha_mask_bake.h"

#include "core/templates/hash_map.h"
#include "core/templates/local_vector.h"

#include "modules/modules_enabled.gen.h" // For msdfgen.

#ifdef MODULE_MSDFGEN_ENABLED
#define MSDFGEN_PUBLIC
GODOT_GCC_WARNING_PUSH_AND_IGNORE("-Wshadow")
GODOT_CLANG_WARNING_PUSH_AND_IGNORE("-Wshadow")
GODOT_MSVC_WARNING_PUSH_AND_IGNORE(4458) // Declaration hides a class member.
#include "thirdparty/msdfgen/msdfgen.h"
GODOT_MSVC_WARNING_POP
GODOT_CLANG_WARNING_POP
GODOT_GCC_WARNING_POP
#endif

#ifdef MODULE_MSDFGEN_ENABLED

namespace {

constexpr float THRESHOLD = 0.5f;

struct EdgeLinks {
	uint64_t a = UINT64_MAX;
	uint64_t b = UINT64_MAX;
	void add(uint64_t p_other) {
		if (a == UINT64_MAX) {
			a = p_other;
		} else {
			b = p_other;
		}
	}
};

// Douglas-Peucker on an open polyline [p_from, p_to] of p_points: keeps the points further than p_tolerance.
void simplify(const LocalVector<Vector2> &p_points, uint32_t p_from, uint32_t p_to, float p_tolerance, LocalVector<bool> &r_keep) {
	if (p_to <= p_from + 1) {
		return;
	}
	const Vector2 a = p_points[p_from];
	const Vector2 b = p_points[p_to];
	const Vector2 ab = b - a;
	const float len = ab.length();
	float max_d = -1.0f;
	uint32_t max_i = p_from;
	for (uint32_t i = p_from + 1; i < p_to; i++) {
		const Vector2 ap = p_points[i] - a;
		const float d = len > 1e-6f ? Math::abs(ab.cross(ap)) / len : ap.length();
		if (d > max_d) {
			max_d = d;
			max_i = i;
		}
	}
	if (max_d > p_tolerance) {
		r_keep[max_i] = true;
		simplify(p_points, p_from, max_i, p_tolerance, r_keep);
		simplify(p_points, max_i, p_to, p_tolerance, r_keep);
	}
}

} // namespace

Ref<Image> alpha_mask_bake(const Ref<Image> &p_source, const Size2i &p_size, bool p_msdf, float p_range) {
	ERR_FAIL_COND_V(p_source.is_null() || p_source->is_empty(), Ref<Image>());
	ERR_FAIL_COND_V(p_size.x < 1 || p_size.y < 1 || p_size.x > 8192 || p_size.y > 8192, Ref<Image>());
	ERR_FAIL_COND_V(p_range <= 0.0f, Ref<Image>());

	// Coverage of the source: its alpha, or the red channel of an image without alpha (a grey mask).
	Ref<Image> src = p_source->duplicate();
	if (src->is_compressed()) {
		ERR_FAIL_COND_V(src->decompress() != OK, Ref<Image>());
	}
	const bool use_alpha = src->detect_alpha() != Image::ALPHA_NONE;
	src->convert(Image::FORMAT_RGBA8);
	const int w = src->get_width();
	const int h = src->get_height();
	const uint8_t *px = src->ptr();
	LocalVector<float> v;
	v.resize((w + 2) * (h + 2));
	for (int y = -1; y <= h; y++) {
		for (int x = -1; x <= w; x++) {
			float f = 0.0f;
			if (x >= 0 && y >= 0 && x < w && y < h) {
				f = px[(y * w + x) * 4 + (use_alpha ? 3 : 0)] / 255.0f;
			}
			v[(y + 1) * (w + 2) + (x + 1)] = f;
		}
	}
	auto val = [&](int p_x, int p_y) -> float { return v[(p_y + 1) * (w + 2) + (p_x + 1)]; };

	// Marching squares over the pixel centres (x + 0.5, y + 0.5), padded with 0 so that every contour is closed.
	// Edge keys: horizontal edge (x, y)-(x + 1, y) and vertical edge (x, y)-(x, y + 1).
	const uint64_t stride = uint64_t(w + 2);
	auto key_h = [&](int p_x, int p_y) -> uint64_t { return ((uint64_t(p_y + 1) * stride + uint64_t(p_x + 1)) << 1); };
	auto key_v = [&](int p_x, int p_y) -> uint64_t { return ((uint64_t(p_y + 1) * stride + uint64_t(p_x + 1)) << 1) | 1; };
	HashMap<uint64_t, EdgeLinks> links;
	HashMap<uint64_t, Vector2> points;
	auto point = [&](uint64_t p_key) {
		if (points.has(p_key)) {
			return;
		}
		const uint64_t cell = p_key >> 1;
		const int x = int(cell % stride) - 1;
		const int y = int(cell / stride) - 1;
		const float fa = val(x, y);
		const float fb = (p_key & 1) ? val(x, y + 1) : val(x + 1, y);
		const float t = CLAMP((THRESHOLD - fa) / (fb - fa), 0.0f, 1.0f);
		points.insert(p_key, (p_key & 1) ? Vector2(x + 0.5f, y + 0.5f + t) : Vector2(x + 0.5f + t, y + 0.5f));
	};
	auto segment = [&](uint64_t p_a, uint64_t p_b) {
		point(p_a);
		point(p_b);
		links[p_a].add(p_b);
		links[p_b].add(p_a);
	};
	for (int y = -1; y < h; y++) {
		for (int x = -1; x < w; x++) {
			const float tl = val(x, y), tr = val(x + 1, y), br = val(x + 1, y + 1), bl = val(x, y + 1);
			const int c = (tl > THRESHOLD ? 1 : 0) | (tr > THRESHOLD ? 2 : 0) | (br > THRESHOLD ? 4 : 0) | (bl > THRESHOLD ? 8 : 0);
			if (c == 0 || c == 15) {
				continue;
			}
			const uint64_t top = key_h(x, y), bottom = key_h(x, y + 1), left = key_v(x, y), right = key_v(x + 1, y);
			const bool centre_in = (tl + tr + br + bl) * 0.25f > THRESHOLD;
			switch (c) {
				case 1:
				case 14:
					segment(left, top);
					break;
				case 2:
				case 13:
					segment(top, right);
					break;
				case 3:
				case 12:
					segment(left, right);
					break;
				case 4:
				case 11:
					segment(right, bottom);
					break;
				case 6:
				case 9:
					segment(top, bottom);
					break;
				case 7:
				case 8:
					segment(left, bottom);
					break;
				case 5: // Top-left and bottom-right inside.
					if (centre_in) {
						segment(top, right);
						segment(left, bottom);
					} else {
						segment(left, top);
						segment(right, bottom);
					}
					break;
				case 10: // Top-right and bottom-left inside.
					if (centre_in) {
						segment(left, top);
						segment(right, bottom);
					} else {
						segment(top, right);
						segment(left, bottom);
					}
					break;
			}
		}
	}
	ERR_FAIL_COND_V_MSG(links.is_empty(), Ref<Image>(), "The source has no contour at alpha 0.5.");

	// Closed loops -> simplified polylines -> msdfgen contours (in output texels).
	const Vector2 scale = Vector2(float(p_size.x) / w, float(p_size.y) / h);
	msdfgen::Shape shape;
	HashMap<uint64_t, bool> visited;
	for (const KeyValue<uint64_t, EdgeLinks> &kv : links) {
		if (visited.has(kv.key)) {
			continue;
		}
		LocalVector<Vector2> loop;
		uint64_t prev = UINT64_MAX;
		uint64_t cur = kv.key;
		while (!visited.has(cur)) {
			visited.insert(cur, true);
			loop.push_back(points[cur]);
			const EdgeLinks &l = links[cur];
			const uint64_t next = (l.a != prev) ? l.a : l.b;
			if (next == UINT64_MAX) {
				break;
			}
			prev = cur;
			cur = next;
		}
		if (loop.size() < 3) {
			continue;
		}
		// Douglas-Peucker on the closed loop: split at the point furthest from the first one.
		uint32_t far_i = 0;
		float far_d = -1.0f;
		for (uint32_t i = 1; i < loop.size(); i++) {
			const float d = loop[i].distance_squared_to(loop[0]);
			if (d > far_d) {
				far_d = d;
				far_i = i;
			}
		}
		loop.push_back(loop[0]);
		LocalVector<bool> keep;
		keep.resize(loop.size());
		for (uint32_t i = 0; i < keep.size(); i++) {
			keep[i] = false;
		}
		keep[0] = keep[far_i] = keep[loop.size() - 1] = true;
		// Source pixels: coarse enough to merge the chamfer marching squares cuts into a sharp corner (two half turns
		// would each stay under the corner threshold), still far below an output texel for a large source.
		const float tolerance = 0.6f;
		simplify(loop, 0, far_i, tolerance, keep);
		simplify(loop, far_i, loop.size() - 1, tolerance, keep);
		LocalVector<Vector2> kept;
		for (uint32_t i = 0; i + 1 < loop.size(); i++) {
			if (keep[i]) {
				kept.push_back(loop[i] * scale);
			}
		}
		if (kept.size() < 3) {
			continue;
		}
		msdfgen::Contour &contour = shape.addContour();
		for (uint32_t i = 0; i < kept.size(); i++) {
			const Vector2 a = kept[i];
			const Vector2 b = kept[(i + 1) % kept.size()];
			contour.addEdge(msdfgen::EdgeHolder(msdfgen::Point2(a.x, a.y), msdfgen::Point2(b.x, b.y)));
		}
	}
	ERR_FAIL_COND_V_MSG(shape.contours.empty(), Ref<Image>(), "The source's contours are too small.");
	shape.normalize();
	shape.orientContours();
	// Corners: turns over 40 degrees (msdfgen's threshold is sin(angle) on the cross product; glyphs use 3.0, i.e. ~8
	// degrees, which on a polyline that follows a curve makes nearly every vertex a corner).
	msdfgen::edgeColoringSimple(shape, Math::deg_to_rad(40.0));

	const msdfgen::Projection projection(msdfgen::Vector2(1.0, 1.0), msdfgen::Vector2(0.0, 0.0));
	const msdfgen::Range range(2.0 * p_range);
	const int ow = p_size.x;
	const int oh = p_size.y;
	const int channels = p_msdf ? 3 : 1;
	LocalVector<float> out;
	out.resize(ow * oh * channels);
	if (p_msdf) {
		msdfgen::Bitmap<float, 3> bitmap(ow, oh);
		msdfgen::MSDFGeneratorConfig config(true, msdfgen::ErrorCorrectionConfig());
		msdfgen::generateMSDF(bitmap, shape, projection, range, config);
		for (int y = 0; y < oh; y++) {
			for (int x = 0; x < ow; x++) {
				for (int c = 0; c < 3; c++) {
					out[(y * ow + x) * 3 + c] = bitmap(x, y)[c];
				}
			}
		}
	} else {
		msdfgen::Bitmap<float, 1> bitmap(ow, oh);
		msdfgen::generateSDF(bitmap, shape, projection, range);
		for (int y = 0; y < oh; y++) {
			for (int x = 0; x < ow; x++) {
				out[y * ow + x] = *bitmap(x, y);
			}
		}
	}

	// The bitmap's row order and the sign convention are msdfgen's: pick the orientation that agrees with the
	// source (inside above 0.5) at the texel centres.
	auto value = [&](int p_x, int p_y, bool p_flip) -> float {
		const int y = p_flip ? oh - 1 - p_y : p_y;
		if (channels == 1) {
			return out[y * ow + p_x];
		}
		const float *m = &out[(y * ow + p_x) * 3];
		return MAX(MIN(m[0], m[1]), MIN(MAX(m[0], m[1]), m[2]));
	};
	int64_t score[2][2] = { { 0, 0 }, { 0, 0 } };
	for (int y = 0; y < oh; y++) {
		for (int x = 0; x < ow; x++) {
			const int sx = CLAMP(int((x + 0.5f) / scale.x), 0, w - 1);
			const int sy = CLAMP(int((y + 0.5f) / scale.y), 0, h - 1);
			const bool inside = val(sx, sy) > THRESHOLD;
			for (int f = 0; f < 2; f++) {
				const bool above = value(x, y, f) > 0.5f;
				score[f][0] += (above == inside) ? 1 : 0;
				score[f][1] += (above != inside) ? 1 : 0;
			}
		}
	}
	int best_f = 0, best_i = 0;
	for (int f = 0; f < 2; f++) {
		for (int i = 0; i < 2; i++) {
			if (score[f][i] > score[best_f][best_i]) {
				best_f = f;
				best_i = i;
			}
		}
	}

	Vector<uint8_t> data;
	data.resize(ow * oh * channels);
	uint8_t *wr = data.ptrw();
	for (int y = 0; y < oh; y++) {
		const int sy = best_f ? oh - 1 - y : y;
		for (int x = 0; x < ow; x++) {
			for (int c = 0; c < channels; c++) {
				float f = out[(sy * ow + x) * channels + c];
				if (best_i) {
					f = 1.0f - f;
				}
				wr[(y * ow + x) * channels + c] = uint8_t(CLAMP(Math::round(f * 255.0f), 0.0f, 255.0f));
			}
		}
	}
	return Image::create_from_data(ow, oh, false, p_msdf ? Image::FORMAT_RGB8 : Image::FORMAT_L8, data);
}

#else

Ref<Image> alpha_mask_bake(const Ref<Image> &p_source, const Size2i &p_size, bool p_msdf, float p_range) {
	ERR_FAIL_V_MSG(Ref<Image>(), "Baking an alpha mask needs the msdfgen module.");
}

#endif // MODULE_MSDFGEN_ENABLED
