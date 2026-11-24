/**************************************************************************/
/*  test_mariner_au4.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_au4)

#include "core/math/random_pcg.h"
#include "servers/audio/audio_server.h"
#include "servers/audio/effects/audio_effect_convolution.h"

#include <vector>

// Mariner engine patch AU4: AudioEffectConvolution — partitioned FFT convolution with a given impulse response.

namespace TestMarinerAU4 {

static std::vector<float> run(const Ref<AudioEffectConvolution> &p_fx, const std::vector<float> &p_in) {
	Ref<AudioEffectInstance> ins = p_fx->instantiate();
	std::vector<float> out(p_in.size());
	const int chunk = 300; // not a multiple of the block
	std::vector<AudioFrame> src(chunk), dst(chunk);
	for (size_t o = 0; o < p_in.size(); o += chunk) {
		const int n = (int)MIN((size_t)chunk, p_in.size() - o);
		for (int i = 0; i < n; i++) {
			src[i] = AudioFrame(p_in[o + i], p_in[o + i]);
		}
		ins->process(src.data(), dst.data(), n);
		for (int i = 0; i < n; i++) {
			out[o + i] = dst[i].left;
		}
	}
	return out;
}

TEST_CASE("[Mariner][AU4] FFT round trip") {
	const int n = 512;
	std::vector<float> a(2 * n), b;
	RandomPCG rng(7);
	for (int i = 0; i < 2 * n; i++) {
		a[i] = rng.randf() - 0.5f;
	}
	b = a;
	AudioEffectConvolution::fft(b.data(), n, false);
	AudioEffectConvolution::fft(b.data(), n, true);
	double err = 0.0;
	for (int i = 0; i < 2 * n; i++) {
		err = MAX(err, (double)Math::abs(b[i] / n - a[i]));
	}
	CHECK(err < 1e-5);
}

TEST_CASE("[Mariner][AU4] convolution equals direct convolution, one block late") {
	const float rate = AudioServer::get_singleton() ? AudioServer::get_singleton()->get_mix_rate() : 48000.0f;
	RandomPCG rng(11);
	PackedFloat32Array ir;
	const int L = 1000;
	ir.resize(L);
	for (int i = 0; i < L; i++) {
		ir.set(i, (rng.randf() - 0.5f) * Math::exp(-i / 300.0));
	}
	Ref<AudioEffectConvolution> fx;
	fx.instantiate();
	fx->set_impulse(ir, PackedFloat32Array(), rate);
	fx->set_wet(1.0f);
	fx->set_dry(0.0f);
	CHECK(fx->get_partition_count() == (L + AudioEffectConvolution::BLOCK - 1) / AudioEffectConvolution::BLOCK);
	std::vector<float> x(4000);
	for (size_t i = 0; i < x.size(); i++) {
		x[i] = rng.randf() - 0.5f;
	}
	const std::vector<float> y = run(fx, x);
	const int B = AudioEffectConvolution::BLOCK;
	double err = 0.0;
	double ref = 0.0;
	for (int n = B; n < (int)x.size(); n++) {
		double s = 0.0;
		for (int k = 0; k < L && k <= n - B; k++) {
			s += (double)ir[k] * x[n - B - k];
		}
		err = MAX(err, (double)Math::abs(s - (double)y[n]));
		ref = MAX(ref, (double)Math::abs(s));
	}
	INFO("max error ", err, " of ", ref);
	CHECK(err < 1e-3 * ref);
}

} // namespace TestMarinerAU4
