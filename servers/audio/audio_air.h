/**************************************************************************/
/*  audio_air.h                                                           */
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

#include <cmath>

// Mariner AU1: sound in open air.
//
// Air absorbs sound through viscosity, heat conduction and the relaxation of oxygen and nitrogen molecules; the loss
// in dB per metre grows roughly with the square of the frequency (ISO 9613-1). At 20 C and 50 % humidity: 1 kHz
// 4.7 dB/km, 4 kHz 30 dB/km, 8 kHz 105 dB/km - a far shot loses its crack and keeps its thump.
//
// A voice is filtered by AIR_STAGES equal one-pole low-pass stages. Their cutoff for a distance is fitted to the
// ISO curve (least squares in dB over 250 Hz - 12 kHz, losses clipped at -40 dB: deeper
// they are masked by the rest of the sound, and fitting them costs accuracy at 2-4 kHz) for the current conditions and mix
// rate, at FIT_DISTANCES distances; between them the cutoff is interpolated in log-log. The fit runs on the main
// thread when the conditions change, not per voice.

namespace AudioAir {

static constexpr int AIR_STAGES = 8; // RMS error of the fit (20 C, 50 %): 0.6 dB at 100 m, 1.2-1.3 dB at 300 m - 1 km.
static constexpr double FIT_FLOOR_DB = -40.0;
static constexpr int LOWPASS_STAGES = 2; // The free low-pass of a voice (occlusion): 12 dB per octave.
static constexpr int FIT_DISTANCES = 24;
static constexpr double FIT_MIN_DISTANCE = 5.0;
static constexpr double FIT_MAX_DISTANCE = 5000.0;

struct Conditions {
	double temperature_c = 20.0;
	double humidity = 50.0; // Relative, %.
	double pressure_kpa = 101.325;
};

// ISO 9613-1 pure-tone absorption coefficient, dB per metre.
inline double absorption_db_per_m(double p_frequency_hz, const Conditions &p_c) {
	const double pr = 101.325;
	const double t0 = 293.15;
	const double t01 = 273.16;
	const double pa = p_c.pressure_kpa > 1.0 ? p_c.pressure_kpa : pr;
	const double t = p_c.temperature_c + 273.15;
	const double rh = p_c.humidity < 0.0 ? 0.0 : (p_c.humidity > 100.0 ? 100.0 : p_c.humidity);
	const double c_sat = -6.8346 * std::pow(t01 / t, 1.261) + 4.6151;
	const double h = rh * std::pow(10.0, c_sat) * (pr / pa); // Molar concentration of water vapour, %.
	const double fr_o = (pa / pr) * (24.0 + 4.04e4 * h * (0.02 + h) / (0.391 + h));
	const double fr_n = (pa / pr) * std::pow(t / t0, -0.5) * (9.0 + 280.0 * h * std::exp(-4.170 * (std::pow(t / t0, -1.0 / 3.0) - 1.0)));
	const double f2 = p_frequency_hz * p_frequency_hz;
	const double classic = 1.84e-11 * (pr / pa) * std::sqrt(t / t0);
	const double relax = std::pow(t / t0, -2.5) * (0.01275 * std::exp(-2239.1 / t) / (fr_o + f2 / fr_o) + 0.1068 * std::exp(-3352.0 / t) / (fr_n + f2 / fr_n));
	return 8.686 * f2 * (classic + relax);
}

// Speed of sound in dry air, m/s.
inline double speed_of_sound(const Conditions &p_c) {
	const double t = p_c.temperature_c < -100.0 ? -100.0 : p_c.temperature_c;
	return 331.3 * std::sqrt(1.0 + t / 273.15);
}

// Coefficient of a one-pole stage y += a * (x - y); 1 means "pass through".
inline double stage_coefficient(double p_cutoff_hz, double p_mix_rate) {
	if (!(p_cutoff_hz > 0.0) || p_cutoff_hz >= p_mix_rate * 0.45) {
		return 1.0;
	}
	return 1.0 - std::exp(-6.283185307179586 * p_cutoff_hz / p_mix_rate);
}

// Gain of p_stages such stages at a frequency, dB.
inline double filter_db(double p_cutoff_hz, double p_frequency_hz, double p_mix_rate, int p_stages) {
	const double a = stage_coefficient(p_cutoff_hz, p_mix_rate);
	if (a >= 1.0) {
		return 0.0;
	}
	const double w = 6.283185307179586 * p_frequency_hz / p_mix_rate;
	const double re = 1.0 - (1.0 - a) * std::cos(w);
	const double im = (1.0 - a) * std::sin(w);
	return p_stages * 20.0 * std::log10(a / std::sqrt(re * re + im * im));
}

inline double fit_distance(int p_i) {
	return FIT_MIN_DISTANCE * std::pow(FIT_MAX_DISTANCE / FIT_MIN_DISTANCE, double(p_i) / double(FIT_DISTANCES - 1));
}

// RMS error in dB of the air filter at p_cutoff_hz against ISO 9613-1 over p_distance metres.
inline double fit_error_db(double p_cutoff_hz, double p_distance, const Conditions &p_c, double p_mix_rate) {
	static constexpr int N = 24;
	double sum = 0.0;
	for (int i = 0; i < N; i++) {
		const double f = 250.0 * std::pow(12000.0 / 250.0, double(i) / double(N - 1));
		double target = -absorption_db_per_m(f, p_c) * p_distance;
		double got = filter_db(p_cutoff_hz, f, p_mix_rate, AIR_STAGES);
		target = target < FIT_FLOOR_DB ? FIT_FLOOR_DB : target;
		got = got < FIT_FLOOR_DB ? FIT_FLOOR_DB : got;
		sum += (got - target) * (got - target);
	}
	return std::sqrt(sum / N);
}

// Best cutoff for a distance (golden section search in log frequency); 0 = no filter needed.
inline double fit_cutoff_hz(double p_distance, const Conditions &p_c, double p_mix_rate) {
	const double top = p_mix_rate * 0.45;
	if (absorption_db_per_m(12000.0, p_c) * p_distance < 0.5) {
		return 0.0;
	}
	double lo = std::log(80.0);
	double hi = std::log(top);
	const double g = 0.6180339887498949;
	double a = hi - g * (hi - lo);
	double b = lo + g * (hi - lo);
	double ea = fit_error_db(std::exp(a), p_distance, p_c, p_mix_rate);
	double eb = fit_error_db(std::exp(b), p_distance, p_c, p_mix_rate);
	for (int it = 0; it < 32; it++) {
		if (ea < eb) {
			hi = b;
			b = a;
			eb = ea;
			a = hi - g * (hi - lo);
			ea = fit_error_db(std::exp(a), p_distance, p_c, p_mix_rate);
		} else {
			lo = a;
			a = b;
			ea = eb;
			b = lo + g * (hi - lo);
			eb = fit_error_db(std::exp(b), p_distance, p_c, p_mix_rate);
		}
	}
	const double fc = std::exp(0.5 * (lo + hi));
	return fc >= top * 0.99 ? 0.0 : fc;
}

struct Table {
	float cutoff_hz[FIT_DISTANCES] = {}; // 0 = no filter at this distance.

	void build(const Conditions &p_c, double p_mix_rate) {
		for (int i = 0; i < FIT_DISTANCES; i++) {
			cutoff_hz[i] = float(fit_cutoff_hz(fit_distance(i), p_c, p_mix_rate));
		}
	}

	// Cutoff for a distance; 0 = no filter.
	float lookup(double p_distance) const {
		if (!(p_distance > FIT_MIN_DISTANCE)) {
			return cutoff_hz[0];
		}
		const double x = std::log(p_distance / FIT_MIN_DISTANCE) / std::log(FIT_MAX_DISTANCE / FIT_MIN_DISTANCE) * (FIT_DISTANCES - 1);
		if (x >= FIT_DISTANCES - 1) {
			// Past the table the loss keeps growing as the distance: cutoff ~ d^-0.5 for a quadratic law.
			const float last = cutoff_hz[FIT_DISTANCES - 1];
			return last > 0.0f ? float(last * std::sqrt(FIT_MAX_DISTANCE / p_distance)) : 0.0f;
		}
		const int i = int(x);
		const double t = x - i;
		const float a = cutoff_hz[i];
		const float b = cutoff_hz[i + 1];
		if (a <= 0.0f || b <= 0.0f) {
			// Edge of the "no filter" range: the filter starts at the first fitted distance.
			return t < 0.5 ? a : b;
		}
		return float(a * std::pow(double(b) / double(a), t));
	}
};

// Mariner AU1b: one parametric band of a voice (peaking EQ, Audio EQ Cookbook, R. Bristow-Johnson): gain_db at f0,
// width by Q; for a ground dip (ISO 9613-2: porous ground takes 125 Hz - 1 kHz away) the gain is negative.
struct Peak {
	float b0 = 1.0f;
	float b1 = 0.0f;
	float b2 = 0.0f;
	float a1 = 0.0f;
	float a2 = 0.0f;
	bool is_identity() const { return b0 == 1.0f && b1 == 0.0f && b2 == 0.0f && a1 == 0.0f && a2 == 0.0f; }
};

// Coefficients normalized by a0; identity when the band is off (|gain| < 0.05 dB, f0 outside 20 Hz - 0.45 rate).
inline Peak peak_coeffs(double p_f0, double p_gain_db, double p_q, double p_rate) {
	Peak k;
	if (!(p_f0 > 20.0) || p_f0 >= p_rate * 0.45 || (p_gain_db < 0.05 && p_gain_db > -0.05)) {
		return k;
	}
	const double q = p_q > 0.1 ? p_q : 0.1;
	const double A = std::pow(10.0, p_gain_db / 40.0);
	const double w0 = 6.283185307179586 * p_f0 / p_rate;
	const double alpha = std::sin(w0) / (2.0 * q);
	const double c = std::cos(w0);
	const double a0 = 1.0 + alpha / A;
	k.b0 = float((1.0 + alpha * A) / a0);
	k.b1 = float(-2.0 * c / a0);
	k.b2 = float((1.0 - alpha * A) / a0);
	k.a1 = float(-2.0 * c / a0);
	k.a2 = float((1.0 - alpha / A) / a0);
	return k;
}

// Gain of the band at a frequency, dB.
inline double peak_db(const Peak &p_k, double p_f, double p_rate) {
	const double w = 6.283185307179586 * p_f / p_rate;
	// H(e^jw) = (b0 + b1 z^-1 + b2 z^-2) / (1 + a1 z^-1 + a2 z^-2)
	const double c1 = std::cos(w), s1 = std::sin(w), c2 = std::cos(2.0 * w), s2 = std::sin(2.0 * w);
	const double nr = p_k.b0 + p_k.b1 * c1 + p_k.b2 * c2, ni = -(p_k.b1 * s1 + p_k.b2 * s2);
	const double dr = 1.0 + p_k.a1 * c1 + p_k.a2 * c2, di = -(p_k.a1 * s1 + p_k.a2 * s2);
	return 10.0 * std::log10((nr * nr + ni * ni) / (dr * dr + di * di));
}

} // namespace AudioAir
