#[compute]

#version 450

#VERSION_DEFINES

// Mariner G2: auto exposure from a histogram of the log luminance (CameraAttributes.auto_exposure_mode = Histogram).
// Own implementation after the published method (histogram with percentile limits, e.g. CryEngine's AutoExposure and
// Unreal's histogram exposure).
// BUILD   - the luminance of every pixel (the largest channel, as the average mode) into 64 bins over log2, weighted by
//           a Gaussian around the center of the frame (center_weight); shared bins per group, then the global ones.
//           Each bin also sums where in the bin its pixels are (1/16 of a bin): a bin spans a third of a stop or more,
//           and the mean must not snap to bin centers.
// RESOLVE - one group: the weighted mean log luminance between the low and high percentiles of the weight (the darker
//           part and the brightest outliers - the sun, lamps, a window - left out), adapted from the previous value in
//           log space with its own speed toward brighter and toward darker; clamped to the minimum and maximum, written
//           as the 1x1 luminance the tone mapper reads (as the average mode); the bins cleared for the next frame.

#define BINS 64

#ifdef MODE_BUILD
layout(local_size_x = 16, local_size_y = 16, local_size_z = 1) in;
#else
layout(local_size_x = BINS, local_size_y = 1, local_size_z = 1) in;
#endif

layout(set = 0, binding = 0, std430) restrict buffer Histogram {
	uint bins[BINS]; // Weight x 64.
	uint offsets[BINS]; // Weight x 8 x position in the bin x 16.
}
histogram;

#ifdef MODE_BUILD
layout(set = 1, binding = 0) uniform sampler2D source_texture;
#else
layout(set = 1, binding = 0) uniform sampler2D prev_luminance;
layout(r32f, set = 2, binding = 0) uniform restrict writeonly image2D dest_luminance;
#endif

layout(push_constant, std430) uniform Params {
	ivec2 source_size;
	float log_min; // log2 of the luminance at the bottom of bin 0.
	float log_range; // log2 span of all the bins.

	float center_weight;
	float low_percentile;
	float high_percentile;
	float min_luminance;

	float max_luminance;
	float adapt_bright; // 1 - exp(-speed * time step) toward a brighter scene.
	float adapt_dark; // The same toward a darker one.
	uint set_immediate;
}
params;

shared uint local_bins[BINS];
shared uint local_offsets[BINS];

#ifdef MODE_BUILD
void main() {
	uint t = gl_LocalInvocationIndex;
	if (t < BINS) {
		local_bins[t] = 0;
		local_offsets[t] = 0;
	}
	barrier();

	ivec2 pos = ivec2(gl_GlobalInvocationID.xy);
	if (all(lessThan(pos, params.source_size))) {
		vec3 c = texelFetch(source_texture, pos, 0).rgb;
		float l = max(c.r, max(c.g, c.b));
		uint bin = 0;
		float position = 0.0;
		if (l > 0.0) {
			float b = clamp((log2(l) - params.log_min) / params.log_range * float(BINS), 0.0, float(BINS) - 0.001);
			bin = uint(b);
			position = fract(b);
		}
		vec2 d = (vec2(pos) + 0.5) / vec2(params.source_size) * 2.0 - 1.0;
		d.x *= float(params.source_size.x) / float(params.source_size.y);
		float w = mix(1.0, exp(-2.0 * dot(d, d)), params.center_weight);
		atomicAdd(local_bins[bin], uint(w * 64.0 + 0.5));
		atomicAdd(local_offsets[bin], uint(w * 8.0 * position * 16.0 + 0.5));
	}
	barrier();

	if (t < BINS && local_bins[t] > 0) {
		atomicAdd(histogram.bins[t], local_bins[t]);
		atomicAdd(histogram.offsets[t], local_offsets[t]);
	}
}
#endif

#ifdef MODE_RESOLVE
void main() {
	uint t = gl_LocalInvocationIndex;
	local_bins[t] = histogram.bins[t];
	local_offsets[t] = histogram.offsets[t];
	barrier();

	if (t == 0) {
		float total = 0.0;
		for (int i = 0; i < BINS; i++) {
			total += float(local_bins[i]);
		}
		float prev = max(texelFetch(prev_luminance, ivec2(0, 0), 0).r, 1e-8);
		float result = prev;
		if (total > 0.0) {
			float low = total * params.low_percentile;
			float high = total * params.high_percentile;
			float acc = 0.0;
			float sum_w = 0.0;
			float sum_l = 0.0;
			for (int i = 0; i < BINS; i++) {
				float h = float(local_bins[i]);
				float w = clamp(min(acc + h, high) - max(acc, low), 0.0, h);
				sum_w += w;
				// The mean position in the bin: offsets / (bins / 64 x 8 x 16).
				float position = h > 0.0 ? clamp(float(local_offsets[i]) * 0.5 / h, 0.0, 1.0) : 0.5;
				sum_l += w * (params.log_min + (float(i) + position) / float(BINS) * params.log_range);
				acc += h;
			}
			if (sum_w > 0.0) {
				float target = sum_l / sum_w;
				if (params.set_immediate != 0) {
					result = exp2(target);
				} else {
					float lp = log2(prev);
					float a = target > lp ? params.adapt_bright : params.adapt_dark;
					result = exp2(mix(lp, target, a));
				}
			}
		}
		imageStore(dest_luminance, ivec2(0, 0), vec4(clamp(result, params.min_luminance, params.max_luminance)));
	}
	barrier();
	histogram.bins[t] = 0;
	histogram.offsets[t] = 0;
}
#endif
