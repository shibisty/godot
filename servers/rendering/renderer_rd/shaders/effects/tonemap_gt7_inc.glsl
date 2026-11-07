// Mariner E23: Gran Turismo 7 tone mapper (Polyphony Digital, SIGGRAPH 2025).
//
// Ported from Spartan Engine, data/shaders/output.hlsl, commit cb258a150 (the last revision under the MIT license),
// which ports the C++ reference code of the presentation. Translated from HLSL to GLSL for Godot (Forward+, Mobile and
// Compatibility include this file); Godot's scene linear (1.0 = SDR paper white) is mapped to GT7's frame buffer units,
// the curve constants that depend only on the display peak are computed on the CPU
// (RendererEnvironmentStorage::environment_get_tonemap_parameters()).
//
// Copyright (c) 2015-2026 Panos Karabelas
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and / or sell
// copies of the Software, and to permit persons to whom the Software is furnished
// to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS
// FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.IN NO EVENT SHALL THE AUTHORS OR
// COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER LIABILITY, WHETHER
// IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN
// CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

// In GT7, 1.0 in the frame buffer is 100 cd/m^2 and SDR paper white is 250 cd/m^2.
#define GT7_PAPER_WHITE_FB 2.5

// GLSL matrices are column-major: mat3(r0, r1, r2) with v * m gives dot(v, r_i), the HLSL mul(M, v) of the rows.
const mat3 gt7_rec709_to_rec2020 = mat3(
		vec3(0.6274040, 0.3292820, 0.0433136),
		vec3(0.0690970, 0.9195400, 0.0113612),
		vec3(0.0163916, 0.0880132, 0.8955950));

const mat3 gt7_rec2020_to_rec709 = mat3(
		vec3(1.6604910, -0.5876411, -0.0728499),
		vec3(-0.1245505, 1.1328999, -0.0083494),
		vec3(-0.0181508, -0.1005789, 1.1187297));

// ST 2084 (PQ) inverse EOTF of frame buffer units (1.0 = 100 cd/m^2): linear light -> perceptual signal.
float gt7_inverse_eotf(float v) {
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	float y = v * (100.0 / 10000.0);
	float ym = pow(max(y, 0.0), m1);
	return exp2(m2 * (log2(c1 + c2 * ym) - log2(1.0 + c3 * ym)));
}

// PQ EOTF: perceptual signal -> frame buffer units.
float gt7_eotf(float n) {
	const float m1 = 0.1593017578125;
	const float m2 = 78.84375;
	const float c1 = 0.8359375;
	const float c2 = 18.8515625;
	const float c3 = 18.6875;
	float np = pow(clamp(n, 0.0, 1.0), 1.0 / m2);
	float l = max(np - c1, 0.0);
	l = l / (c2 - c3 * np);
	l = pow(l, 1.0 / m1);
	return l * (10000.0 / 100.0);
}

// ICtCp (Rec. 2020): intensity apart from chroma, so the highlights can be mapped without hue shifts.
vec3 gt7_rgb_to_ictcp(vec3 rgb) {
	float l = (rgb.r * 1688.0 + rgb.g * 2146.0 + rgb.b * 262.0) / 4096.0;
	float m = (rgb.r * 683.0 + rgb.g * 2951.0 + rgb.b * 462.0) / 4096.0;
	float s = (rgb.r * 99.0 + rgb.g * 309.0 + rgb.b * 3688.0) / 4096.0;

	float lpq = gt7_inverse_eotf(l);
	float mpq = gt7_inverse_eotf(m);
	float spq = gt7_inverse_eotf(s);

	return vec3(
			(2048.0 * lpq + 2048.0 * mpq) / 4096.0,
			(6610.0 * lpq - 13613.0 * mpq + 7003.0 * spq) / 4096.0,
			(17933.0 * lpq - 17390.0 * mpq - 543.0 * spq) / 4096.0);
}

vec3 gt7_ictcp_to_rgb(vec3 ictcp) {
	float l = ictcp.x + 0.00860904 * ictcp.y + 0.11103 * ictcp.z;
	float m = ictcp.x - 0.00860904 * ictcp.y - 0.11103 * ictcp.z;
	float s = ictcp.x + 0.560031 * ictcp.y - 0.320627 * ictcp.z;

	float l_lin = gt7_eotf(l);
	float m_lin = gt7_eotf(m);
	float s_lin = gt7_eotf(s);

	return max(vec3(
					   3.43661 * l_lin - 2.50645 * m_lin + 0.0698454 * s_lin,
					   -0.79133 * l_lin + 1.9836 * m_lin - 0.192271 * s_lin,
					   -0.0259499 * l_lin - 0.0989137 * m_lin + 1.12486 * s_lin),
			vec3(0.0));
}

// The S-curve: toe and linear section below the shoulder, exponential shoulder towards the peak.
float gt7_evaluate_curve(float x, float peak) {
	const float alpha = 0.25;
	const float linear_section = 0.444;
	const float mid_point = 0.538;
	const float toe_strength = 1.280;
	const float k = (linear_section - 1.0) / (alpha - 1.0);

	if (x < 0.0) {
		return 0.0;
	}

	if (x >= linear_section * peak) {
		float ka = peak * linear_section + peak * k;
		float kb = -peak * k * exp(linear_section / k);
		float kc = -1.0 / (k * peak);
		return ka + kb * exp(x * kc);
	}

	float weight_linear = smoothstep(0.0, mid_point, x);
	float toe_mapped = mid_point * pow(max(x / mid_point, 0.0), toe_strength);
	return (1.0 - weight_linear) * toe_mapped + weight_linear * x;
}

// p_color: linear Rec. 709, 1.0 = paper white. p_peak: display peak in frame buffer units (2.5 for SDR, more for HDR
// output), p_peak_intensity: gt7_inverse_eotf(p_peak). Returns linear Rec. 709, 1.0 = paper white, at most
// p_peak / GT7_PAPER_WHITE_FB.
vec3 tonemap_gt7(vec3 p_color, float p_peak, float p_peak_intensity) {
	vec3 rgb = (p_color * GT7_PAPER_WHITE_FB) * gt7_rec709_to_rec2020;

	vec3 ucs = gt7_rgb_to_ictcp(rgb);

	// Path 1: per channel, keeps the saturation, may shift the hue.
	vec3 skewed_rgb = vec3(
			gt7_evaluate_curve(rgb.r, p_peak),
			gt7_evaluate_curve(rgb.g, p_peak),
			gt7_evaluate_curve(rgb.b, p_peak));
	vec3 skewed_ucs = gt7_rgb_to_ictcp(skewed_rgb);

	// Path 2: intensity only, keeps the hue, the highlights fade to white.
	const float fade_start = 0.98;
	const float fade_end = 1.16;
	float chroma_scale = 1.0 - smoothstep(fade_start, fade_end, ucs.x / p_peak_intensity);
	vec3 scaled_rgb = gt7_ictcp_to_rgb(vec3(skewed_ucs.x, ucs.y * chroma_scale, ucs.z * chroma_scale));

	const float blend_ratio = 0.6;
	vec3 out_rgb = min((1.0 - blend_ratio) * skewed_rgb + blend_ratio * scaled_rgb, vec3(p_peak));
	return max((out_rgb * gt7_rec2020_to_rec709) * (1.0 / GT7_PAPER_WHITE_FB), vec3(0.0));
}
