/**************************************************************************/
/*  test_mariner_au3.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_au3)

#include "core/math/math_funcs.h"
#include "servers/audio/effects/reverb_filter.h"

#include <cmath>
#include <vector>

// Mariner engine patch AU3: AudioEffectReverb.decay_time / decay_hf_ratio / decay_hf_reference — exact RT60 of the
// Freeverb combs (room_size gives only 0.6..10 s and no stated time). Used by Terra (addons/terra_sound,
// TerraSoundEnv): room reverb time by Eyring from the probe of the listener's surroundings.

namespace TestMarinerAU3 {

TEST_CASE("[Mariner][AU3] comb feedback and damping give the asked decay") {
	const float rate = 48000.0f;
	const int len = 1557; // Freeverb comb 0 at 44.1 kHz
	float g = 0.0f;
	float d = 0.0f;
	Reverb::decay_coefs(len, rate, 0.8f, 1.0f, 5000.0f, g, d);
	CHECK(d == 0.0f);
	// 60 dB in 0.8 s: per pass 60 * L / (fs * T) dB
	CHECK(20.0 * std::log10((double)g) == doctest::Approx(-60.0 * len / (rate * 0.8)).epsilon(1e-4));

	Reverb::decay_coefs(len, rate, 0.8f, 0.5f, 5000.0f, g, d);
	CHECK(d > 0.0f);
	CHECK(d < 1.0f);
	const double w = Math::TAU * 5000.0 / rate;
	const double h = (1.0 - d) / Math::sqrt(1.0 - 2.0 * d * Math::cos(w) + d * d);
	// at 5 kHz the loop loses as much as for half the time
	CHECK(20.0 * std::log10(g * h) == doctest::Approx(-60.0 * len / (rate * 0.4)).epsilon(1e-3));
}

// Schroeder backward integration, decay time from the -5..-25 dB slope (T20 * 3).
static double rt_of(const std::vector<float> &p_ir, double p_rate) {
	std::vector<double> edc(p_ir.size() + 1, 0.0);
	for (int i = (int)p_ir.size() - 1; i >= 0; i--) {
		edc[i] = edc[i + 1] + (double)p_ir[i] * p_ir[i];
	}
	int a = -1;
	int b = -1;
	for (int i = 0; i < (int)p_ir.size(); i++) {
		const double db = 10.0 * std::log10(edc[i] / edc[0] + 1e-30);
		if (a < 0 && db <= -5.0) {
			a = i;
		}
		if (b < 0 && db <= -25.0) {
			b = i;
			break;
		}
	}
	if (a < 0 || b <= a) {
		return 0.0;
	}
	return 3.0 * (b - a) / p_rate;
}

TEST_CASE("[Mariner][AU3] reverb impulse response decays in decay_time") {
	const float rate = 48000.0f;
	for (float t : { 0.15f, 0.3f, 1.0f, 2.5f }) {
		Reverb r;
		r.set_mix_rate(rate);
		r.set_extra_spread_base(0);
		r.set_extra_spread(1.0);
		r.set_predelay(10);
		r.set_predelay_feedback(0);
		r.set_highpass(0);
		r.set_wet(1.0);
		r.set_dry(0.0);
		r.set_decay(t, 1.0f, 5000.0f);
		const int total = (int)(rate * (t * 1.5f + 0.2f));
		std::vector<float> ir;
		ir.reserve(total);
		float src[Reverb::INPUT_BUFFER_MAX_SIZE];
		float dst[Reverb::INPUT_BUFFER_MAX_SIZE];
		bool first = true;
		while ((int)ir.size() < total) {
			const int n = MIN((int)Reverb::INPUT_BUFFER_MAX_SIZE, total - (int)ir.size());
			for (int i = 0; i < n; i++) {
				src[i] = 0.0f;
			}
			if (first) {
				src[0] = 1.0f;
				first = false;
			}
			r.process(src, dst, n);
			for (int i = 0; i < n; i++) {
				ir.push_back(dst[i]);
			}
		}
		const double rt = rt_of(ir, rate);
		INFO("asked ", t, " s, got ", rt, " s");
		CHECK(rt == doctest::Approx(t).epsilon(0.12));
	}
}

} // namespace TestMarinerAU3
