/**************************************************************************/
/*  test_mariner_au1.cpp                                                  */
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

TEST_FORCE_LINK(test_mariner_au1)

#include "core/object/class_db.h"
#include "scene/3d/audio_stream_player_3d.h"
#include "servers/audio/audio_air.h"

// Mariner engine patch AU1: sound in open air - the air filter of a voice (ISO 9613-1, fitted per conditions and
// mix rate), the propagation delay and a free low-pass on AudioStreamPlayer3D. Listening check: Terra,
// addons/terra_sound (shot at 20 m / 300 m / 1 km, delay against the flash).

namespace TestMarinerAU1 {

TEST_CASE("[Mariner][AU1] ISO 9613-1 absorption and speed of sound") {
	const AudioAir::Conditions c;
	CHECK(AudioAir::absorption_db_per_m(1000.0, c) * 1000.0 == doctest::Approx(4.7).epsilon(0.1));
	CHECK(AudioAir::absorption_db_per_m(4000.0, c) * 1000.0 == doctest::Approx(29.7).epsilon(0.1));
	CHECK(AudioAir::absorption_db_per_m(8000.0, c) * 1000.0 == doctest::Approx(105.0).epsilon(0.1));
	// Grows with frequency.
	CHECK(AudioAir::absorption_db_per_m(500.0, c) < AudioAir::absorption_db_per_m(2000.0, c));

	AudioAir::Conditions frost;
	frost.temperature_c = -5.0;
	frost.humidity = 60.0;
	CHECK(AudioAir::absorption_db_per_m(4000.0, frost) > 1.5 * AudioAir::absorption_db_per_m(4000.0, c));

	CHECK(AudioAir::speed_of_sound(c) == doctest::Approx(343.2).epsilon(0.002));
	CHECK(AudioAir::speed_of_sound(frost) < AudioAir::speed_of_sound(c));
}

TEST_CASE("[Mariner][AU1] air filter table follows the ISO curve") {
	const double rate = 48000.0;
	const AudioAir::Conditions c;
	AudioAir::Table t;
	t.build(c, rate);

	CHECK(t.lookup(5.0) == 0.0f); // Near: no filter.
	float prev = 1e9f;
	for (double d = 20.0; d <= 8000.0; d *= 1.5) {
		const float fc = t.lookup(d);
		CHECK(fc > 0.0f);
		CHECK(fc < prev); // Farther - lower cutoff.
		prev = fc;
	}
	CHECK(AudioAir::fit_error_db(t.lookup(100.0), 100.0, c, rate) < 1.0);
	CHECK(AudioAir::fit_error_db(t.lookup(300.0), 300.0, c, rate) < 2.0);
	CHECK(AudioAir::fit_error_db(t.lookup(1000.0), 1000.0, c, rate) < 2.0);
	CHECK(AudioAir::fit_error_db(t.lookup(3000.0), 3000.0, c, rate) < 3.5);
	// 300 m: crack (8 kHz) mostly gone, body (500 Hz) kept.
	const float fc = t.lookup(300.0);
	CHECK(AudioAir::filter_db(fc, 8000.0, rate, AudioAir::AIR_STAGES) < -20.0);
	CHECK(AudioAir::filter_db(fc, 500.0, rate, AudioAir::AIR_STAGES) > -1.0);

	// Frost absorbs more: lower cutoff at the same distance.
	AudioAir::Conditions frost;
	frost.temperature_c = -5.0;
	frost.humidity = 60.0;
	AudioAir::Table tf;
	tf.build(frost, rate);
	CHECK(tf.lookup(300.0) < t.lookup(300.0));
}

TEST_CASE("[Mariner][AU1] one-pole cascade matches filter_db") {
	const double rate = 48000.0;
	const double fc = 3000.0;
	const double f = 6000.0;
	const float a = (float)AudioAir::stage_coefficient(fc, rate);
	float st[AudioAir::AIR_STAGES] = {};
	double peak = 0.0;
	for (int i = 0; i < 48000; i++) {
		float x = (float)std::sin(6.283185307179586 * f * i / rate);
		for (int s = 0; s < AudioAir::AIR_STAGES; s++) {
			st[s] += a * (x - st[s]);
			x = st[s];
		}
		if (i > 24000) {
			peak = MAX(peak, (double)Math::abs(x));
		}
	}
	CHECK(20.0 * std::log10(peak) == doctest::Approx(AudioAir::filter_db(fc, f, rate, AudioAir::AIR_STAGES)).epsilon(0.02));
	CHECK(AudioAir::stage_coefficient(0.0, rate) == 1.0);
	CHECK(AudioAir::stage_coefficient(rate * 0.46, rate) == 1.0);
}

TEST_CASE("[Mariner][AU1] AudioServer and AudioStreamPlayer3D API") {
	CHECK(ClassDB::has_method("AudioServer", "set_air_conditions"));
	CHECK(ClassDB::has_method("AudioServer", "get_air_conditions"));
	CHECK(ClassDB::has_method("AudioServer", "get_speed_of_sound"));
	CHECK(ClassDB::has_method("AudioServer", "get_air_absorption_db"));
	CHECK(ClassDB::has_method("AudioServer", "get_air_cutoff_hz"));

	// The player itself needs the AudioServer and a scene tree (its constructor uses them: the test run crashed with
	// SIGSEGV here on 21.11), so only its API is checked.
	CHECK(ClassDB::has_method("AudioStreamPlayer3D", "set_air_absorption"));
	CHECK(ClassDB::has_method("AudioStreamPlayer3D", "set_propagation_delay"));
	CHECK(ClassDB::has_method("AudioStreamPlayer3D", "set_lowpass_cutoff_hz"));
}

} // namespace TestMarinerAU1
