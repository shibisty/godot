/**************************************************************************/
/*  audio_effect_convolution.cpp                                          */
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

#include "audio_effect_convolution.h"

#include "core/math/math_funcs.h"
#include "core/object/class_db.h"
#include "servers/audio/audio_server.h"

void AudioEffectConvolution::fft(float *p_re_im, int p_n, bool p_inverse) {
	// bit reversal
	for (int i = 1, j = 0; i < p_n; i++) {
		int bit = p_n >> 1;
		for (; j & bit; bit >>= 1) {
			j ^= bit;
		}
		j ^= bit;
		if (i < j) {
			SWAP(p_re_im[2 * i], p_re_im[2 * j]);
			SWAP(p_re_im[2 * i + 1], p_re_im[2 * j + 1]);
		}
	}
	for (int len = 2; len <= p_n; len <<= 1) {
		const double ang = Math::TAU / len * (p_inverse ? 1.0 : -1.0);
		const float wr = (float)Math::cos(ang);
		const float wi = (float)Math::sin(ang);
		for (int i = 0; i < p_n; i += len) {
			float cr = 1.0f;
			float ci = 0.0f;
			for (int k = 0; k < len / 2; k++) {
				float *a = p_re_im + 2 * (i + k);
				float *b = p_re_im + 2 * (i + k + len / 2);
				const float tr = b[0] * cr - b[1] * ci;
				const float ti = b[0] * ci + b[1] * cr;
				b[0] = a[0] - tr;
				b[1] = a[1] - ti;
				a[0] += tr;
				a[1] += ti;
				const float ncr = cr * wr - ci * wi;
				ci = cr * wi + ci * wr;
				cr = ncr;
			}
		}
	}
}

void AudioEffectConvolution::_prepare() {
	const float mix_rate = AudioServer::get_singleton() ? AudioServer::get_singleton()->get_mix_rate() : 48000.0f;
	Vector<float> sp[2];
	int parts = 0;
	const int src_n = impulse_left.size();
	if (src_n > 0 && impulse_rate > 0.0f) {
		// resample to the mix rate (linear)
		const double ratio = (double)impulse_rate / (double)mix_rate;
		int n = (int)Math::floor((src_n - 1) / ratio) + 1;
		n = MIN(n, (int)(mix_rate * MAX_SECONDS));
		parts = (n + BLOCK - 1) / BLOCK;
		for (int ch = 0; ch < 2; ch++) {
			const PackedFloat32Array &src = (ch == 1 && impulse_right.size() == src_n) ? impulse_right : impulse_left;
			const float *s = src.ptr();
			sp[ch].resize(parts * FFT_SIZE * 2);
			float *w = sp[ch].ptrw();
			for (int p = 0; p < parts; p++) {
				float *f = w + p * FFT_SIZE * 2;
				for (int i = 0; i < FFT_SIZE * 2; i++) {
					f[i] = 0.0f;
				}
				for (int i = 0; i < BLOCK; i++) {
					const int o = p * BLOCK + i;
					if (o >= n) {
						break;
					}
					const double x = o * ratio;
					const int i0 = (int)x;
					const double t = x - i0;
					const float v0 = s[i0];
					const float v1 = (i0 + 1 < src_n) ? s[i0 + 1] : 0.0f;
					f[2 * i] = (float)(v0 + (v1 - v0) * t);
				}
				fft(f, FFT_SIZE, false);
			}
		}
	}
	MutexLock lock(prepared_mutex);
	spectra[0] = sp[0];
	spectra[1] = sp[1];
	prepared_partitions = parts;
	prepared_version.increment();
}

void AudioEffectConvolutionInstance::_sync() {
	const uint32_t v = base->prepared_version.get();
	if (v == version) {
		return;
	}
	{
		MutexLock lock(base->prepared_mutex);
		spectra[0] = base->spectra[0];
		spectra[1] = base->spectra[1];
		partitions = base->prepared_partitions;
		version = v;
	}
	const int B = AudioEffectConvolution::BLOCK;
	const int N = AudioEffectConvolution::FFT_SIZE;
	for (int ch = 0; ch < 2; ch++) {
		in_block[ch].resize(B);
		prev_block[ch].resize(B);
		out_block[ch].resize(B);
		in_block[ch].fill(0.0f);
		prev_block[ch].fill(0.0f);
		out_block[ch].fill(0.0f);
		fdl[ch].resize(MAX(partitions, 1) * N * 2);
		fdl[ch].fill(0.0f);
	}
	work.resize(N * 2);
	acc.resize(N * 2);
	head = 0;
	fill = 0;
}

void AudioEffectConvolutionInstance::_process_block() {
	const int B = AudioEffectConvolution::BLOCK;
	const int N = AudioEffectConvolution::FFT_SIZE;
	const int P = partitions;
	for (int ch = 0; ch < 2; ch++) {
		float *wk = work.ptrw();
		const float *pb = prev_block[ch].ptr();
		const float *ib = in_block[ch].ptr();
		for (int i = 0; i < B; i++) {
			wk[2 * i] = pb[i];
			wk[2 * i + 1] = 0.0f;
			wk[2 * (B + i)] = ib[i];
			wk[2 * (B + i) + 1] = 0.0f;
		}
		AudioEffectConvolution::fft(wk, N, false);
		float *d = fdl[ch].ptrw();
		float *slot = d + head * N * 2;
		for (int i = 0; i < N * 2; i++) {
			slot[i] = wk[i];
		}
		float *ac = acc.ptrw();
		for (int i = 0; i < N * 2; i++) {
			ac[i] = 0.0f;
		}
		const float *h = spectra[ch].ptr();
		for (int p = 0; p < P; p++) {
			const float *x = d + ((head - p + P) % P) * N * 2;
			const float *hp = h + p * N * 2;
			for (int k = 0; k < N; k++) {
				const float xr = x[2 * k], xi = x[2 * k + 1];
				const float hr = hp[2 * k], hi = hp[2 * k + 1];
				ac[2 * k] += xr * hr - xi * hi;
				ac[2 * k + 1] += xr * hi + xi * hr;
			}
		}
		AudioEffectConvolution::fft(ac, N, true);
		float *ob = out_block[ch].ptrw();
		const float inv = 1.0f / N;
		for (int i = 0; i < B; i++) {
			ob[i] = ac[2 * (B + i)] * inv;
		}
		float *pw = prev_block[ch].ptrw(); // copy, not share: sharing would re-allocate on the next write
		for (int i = 0; i < B; i++) {
			pw[i] = ib[i];
		}
	}
	head = (head + 1) % MAX(P, 1);
}

void AudioEffectConvolutionInstance::process(const AudioFrame *p_src_frames, AudioFrame *p_dst_frames, int p_frame_count) {
	_sync();
	const float wet = base->wet;
	const float dry = base->dry;
	if (partitions <= 0) {
		for (int i = 0; i < p_frame_count; i++) {
			p_dst_frames[i] = p_src_frames[i] * dry;
		}
		return;
	}
	const int B = AudioEffectConvolution::BLOCK;
	for (int i = 0; i < p_frame_count; i++) {
		in_block[0].write[fill] = p_src_frames[i].left;
		in_block[1].write[fill] = p_src_frames[i].right;
		p_dst_frames[i].left = p_src_frames[i].left * dry + out_block[0][fill] * wet;
		p_dst_frames[i].right = p_src_frames[i].right * dry + out_block[1][fill] * wet;
		fill++;
		if (fill == B) {
			_process_block();
			fill = 0;
		}
	}
}

Ref<AudioEffectInstance> AudioEffectConvolution::instantiate() {
	Ref<AudioEffectConvolutionInstance> ins;
	ins.instantiate();
	ins->base = Ref<AudioEffectConvolution>(this);
	ins->_sync();
	return ins;
}

void AudioEffectConvolution::set_impulse(const PackedFloat32Array &p_left, const PackedFloat32Array &p_right, float p_rate) {
	impulse_left = p_left;
	impulse_right = p_right;
	impulse_rate = MAX(p_rate, 1.0f);
	_prepare();
}

PackedFloat32Array AudioEffectConvolution::get_impulse_left() const {
	return impulse_left;
}

PackedFloat32Array AudioEffectConvolution::get_impulse_right() const {
	return impulse_right;
}

float AudioEffectConvolution::get_impulse_rate() const {
	return impulse_rate;
}

void AudioEffectConvolution::set_wet(float p_wet) {
	wet = p_wet;
}

float AudioEffectConvolution::get_wet() const {
	return wet;
}

void AudioEffectConvolution::set_dry(float p_dry) {
	dry = p_dry;
}

float AudioEffectConvolution::get_dry() const {
	return dry;
}

int AudioEffectConvolution::get_partition_count() const {
	return prepared_partitions;
}

void AudioEffectConvolution::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_impulse", "left", "right", "rate"), &AudioEffectConvolution::set_impulse);
	ClassDB::bind_method(D_METHOD("get_impulse_left"), &AudioEffectConvolution::get_impulse_left);
	ClassDB::bind_method(D_METHOD("get_impulse_right"), &AudioEffectConvolution::get_impulse_right);
	ClassDB::bind_method(D_METHOD("get_impulse_rate"), &AudioEffectConvolution::get_impulse_rate);
	ClassDB::bind_method(D_METHOD("set_wet", "amount"), &AudioEffectConvolution::set_wet);
	ClassDB::bind_method(D_METHOD("get_wet"), &AudioEffectConvolution::get_wet);
	ClassDB::bind_method(D_METHOD("set_dry", "amount"), &AudioEffectConvolution::set_dry);
	ClassDB::bind_method(D_METHOD("get_dry"), &AudioEffectConvolution::get_dry);
	ClassDB::bind_method(D_METHOD("get_partition_count"), &AudioEffectConvolution::get_partition_count);

	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "wet", PROPERTY_HINT_RANGE, "0,4,0.001"), "set_wet", "get_wet");
	ADD_PROPERTY(PropertyInfo(Variant::FLOAT, "dry", PROPERTY_HINT_RANGE, "0,1,0.01"), "set_dry", "get_dry");
}
