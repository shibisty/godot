/**************************************************************************/
/*  test_mariner_au1b.cpp                                                 */
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

TEST_FORCE_LINK(test_mariner_au1b)

#include "core/object/class_db.h"
#include "scene/3d/audio_stream_player_3d.h"
#include "servers/audio/audio_air.h"

// Mariner engine patch AU1b: one parametric band (peaking EQ) on a voice, AudioStreamPlayer3D.peak_eq_*. Used by
// Terra (addons/terra_sound, TerraSoundPaths) for the ground dip of ISO 9613-2. Stand: Terra, shots.bat sound_path.

namespace TestMarinerAU1b {

TEST_CASE("[Mariner][AU1b] peaking EQ coefficients") {
	const double rate = 48000.0;
	const AudioAir::Peak k = AudioAir::peak_coeffs(500.0, -12.0, 1.4, rate);
	CHECK_FALSE(k.is_identity());
	CHECK(AudioAir::peak_db(k, 500.0, rate) == doctest::Approx(-12.0).epsilon(0.01));
	CHECK(Math::abs(AudioAir::peak_db(k, 20.0, rate)) < 0.5);
	CHECK(Math::abs(AudioAir::peak_db(k, 12000.0, rate)) < 0.5);
	// Q 1.4 is about an octave: an octave away the dip is well under half of it.
	CHECK(AudioAir::peak_db(k, 1000.0, rate) > -6.0);
	CHECK(AudioAir::peak_db(k, 250.0, rate) > -6.0);
	CHECK(AudioAir::peak_coeffs(500.0, 0.0, 1.4, rate).is_identity());
	CHECK(AudioAir::peak_coeffs(0.0, -12.0, 1.4, rate).is_identity());
}

TEST_CASE("[Mariner][AU1b] biquad matches peak_db") {
	const double rate = 48000.0;
	const AudioAir::Peak k = AudioAir::peak_coeffs(800.0, -9.0, 2.0, rate);
	for (double f : { 400.0, 800.0, 1600.0 }) {
		float x1 = 0, x2 = 0, y1 = 0, y2 = 0;
		double peak = 0.0;
		for (int i = 0; i < 48000; i++) {
			const float x = (float)std::sin(6.283185307179586 * f * i / rate);
			const float y = k.b0 * x + k.b1 * x1 + k.b2 * x2 - k.a1 * y1 - k.a2 * y2;
			x2 = x1;
			x1 = x;
			y2 = y1;
			y1 = y;
			if (i > 24000) {
				peak = MAX(peak, (double)Math::abs(y));
			}
		}
		CHECK(20.0 * std::log10(peak) == doctest::Approx(AudioAir::peak_db(k, f, rate)).epsilon(0.03));
	}
}

TEST_CASE("[Mariner][AU1b] AudioStreamPlayer3D API") {
	CHECK(ClassDB::has_method("AudioStreamPlayer3D", "set_peak_eq_db"));
	AudioStreamPlayer3D *p = memnew(AudioStreamPlayer3D);
	CHECK(p->get_peak_eq_db() == 0.0f);
	CHECK(p->get_peak_eq_hz() == 500.0f);
	CHECK(p->get_peak_eq_q() == 1.0f);
	p->set_peak_eq_db(-80.0f);
	CHECK(p->get_peak_eq_db() == -40.0f);
	p->set_peak_eq_q(0.0f);
	CHECK(p->get_peak_eq_q() == doctest::Approx(0.1f));
	memdelete(p);
}

} // namespace TestMarinerAU1b
