/**************************************************************************/
/*  audio_effect_convolution.h                                            */
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

#include "core/os/mutex.h"
#include "core/templates/safe_refcount.h"
#include "servers/audio/audio_effect.h"

// Mariner AU4: convolution with a measured impulse response (uniformly partitioned overlap-save FFT convolution,
// block 256 frames: the wet output lags by one block, 5.3 ms at 48 kHz). The impulse response is given as float
// samples (one or two channels) with its own sample rate; it is resampled to the mix rate (linear). Used by Terra
// (addons/terra_sound, TerraSoundEnv) for the forest tail from measured forest impulse responses (OpenAIR).

class AudioEffectConvolution;

class AudioEffectConvolutionInstance : public AudioEffectInstance {
	GDCLASS(AudioEffectConvolutionInstance, AudioEffectInstance);
	friend class AudioEffectConvolution;
	Ref<AudioEffectConvolution> base;

	uint32_t version = 0xFFFFFFFF;
	int partitions = 0;
	int head = 0;
	int fill = 0;
	// per channel: input block, previous input block, output block, frequency-domain delay line (partitions × N complex)
	Vector<float> in_block[2];
	Vector<float> prev_block[2];
	Vector<float> out_block[2];
	Vector<float> fdl[2];
	Vector<float> spectra[2]; // copy of the prepared impulse spectra (taken when the version changes)
	Vector<float> work;
	Vector<float> acc;

	void _sync();
	void _process_block();

public:
	virtual void process(const AudioFrame *p_src_frames, AudioFrame *p_dst_frames, int p_frame_count) override;
	// The tail outlives the input: keep convolving after the bus input went silent.
	virtual bool process_silence() const override { return true; }
};

class AudioEffectConvolution : public AudioEffect {
	GDCLASS(AudioEffectConvolution, AudioEffect);
	friend class AudioEffectConvolutionInstance;

public:
	enum {
		BLOCK = 256,
		FFT_SIZE = 512,
		MAX_SECONDS = 12,
	};

private:
	PackedFloat32Array impulse_left;
	PackedFloat32Array impulse_right;
	float impulse_rate = 48000.0f;
	float wet = 1.0f;
	float dry = 0.0f;

	// prepared spectra: [channel] → partitions × FFT_SIZE complex (re, im)
	Mutex prepared_mutex;
	Vector<float> spectra[2];
	int prepared_partitions = 0;
	SafeNumeric<uint32_t> prepared_version;

	void _prepare();

protected:
	static void _bind_methods();

public:
	// In place, interleaved complex (re, im), p_n a power of two. Inverse is not scaled.
	static void fft(float *p_re_im, int p_n, bool p_inverse);

	void set_impulse(const PackedFloat32Array &p_left, const PackedFloat32Array &p_right, float p_rate);
	PackedFloat32Array get_impulse_left() const;
	PackedFloat32Array get_impulse_right() const;
	float get_impulse_rate() const;
	void set_wet(float p_wet);
	float get_wet() const;
	void set_dry(float p_dry);
	float get_dry() const;
	int get_partition_count() const;

	Ref<AudioEffectInstance> instantiate() override;
};
