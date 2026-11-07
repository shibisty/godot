#[compute]

#version 450

#VERSION_DEFINES

#include "../motion_vector_inc.glsl"
#define FFX_FSR3UPSCALER_OPTION_GODOT_DERIVE_INVALID_MOTION_VECTORS_FUNCTION(i, j, k) derive_motion_vector(i, j, k)

#include "thirdparty/amd-fsr3/shaders/ffx_fsr3upscaler_shading_change_pass.glsl"
