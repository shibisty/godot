# Godot Engine — Mariner fork

A fork of [Godot Engine](https://github.com/godotengine/godot) for *Mariner Simulator* and our future projects.
It is based on **Godot 4.7.2-stable** and adds rendering and `RenderingDevice` extensions:
- upscaling and frame generation: DLSS, DLSS-G and Reflex, AMD FSR 3.1 and FSR 3 frame generation, dynamic resolution;
- ray tracing: a path tracer, DDGI probes, ReSTIR for many lights, ray-traced sun shadows;
- shadows: a shadow cache, a per-object shadow map, contact shadows, colored translucent shadows;
- GPU-driven static geometry: culling, occlusion and LOD on the GPU;
- post-processing: GTAO, motion blur, lens flare, color grading;
- materials: multiple-scattering diffuse and micro-shadowing;
- `RenderingDevice`: async compute, mesh shaders, compute on a second GPU;
- texture mip streaming;
- fixes to stock bugs.

The original Godot README is in [`README.godot.md`](README.godot.md). The license is the same, MIT
([`LICENSE.txt`](LICENSE.txt)). Third-party SDKs keep their own licenses (see "Third-party code").

**Compatibility.** A project written for stock Godot 4.7 opens and runs in this fork unchanged. New features only
apply where they are enabled: shader render modes, project settings, node, viewport and environment properties, new
methods. New enum values are appended at the end. Most features are Forward+ only; Mobile and Compatibility keep
their stock behaviour, except for the items listed below.

Exceptions, where a stock project looks or behaves differently:
- **M1–M3 are on by default** (`rendering/shading/overrides/multiscatter_diffuse`, `micro_shadowing`,
  `multi_bounce_ao`): materials with the default Burley diffuse and materials with an AO map are lit a little
  differently, in Forward+ and Mobile. Turn the three settings off for stock shading.
- **Fixes of stock bugs** (E9, E14a, E16, E25c, `0040`) change behaviour where stock Godot was wrong: leaks, reads
  past the end of buffers, lost uniform sets, missing depth passes.

### Detecting the fork

Every build of this fork carries the extensions, so it can be recognized without any special build flags:

* the feature tag **`mariner`** is always present: in the editor, in export templates, and with any `BUILD_NAME`;
* the default build name is `mariner` (`Engine.get_version_info().build`). Setting `BUILD_NAME` overrides it, so
  don't rely on the build name alone.

```gdscript
var mariner := OS.has_feature("mariner")
# For a specific capability, check the API directly. Stock Godot doesn't have the method:
var has_mesh := ClassDB.class_has_method("RenderingDevice", "draw_list_draw_mesh_tasks")
```

The feature tag also works in project setting overrides, e.g. `rendering/foo.mariner = true`.

Check new methods with `ClassDB` / `Engine.has_singleton` and call them through `call()`. Then the same game code also
runs on stock Godot.

---

## What differs from vanilla 4.7.2

| # | what | main API | drivers |
|---|---|---|---|
| **E1** | `PREVIOUS_FRAME` vertex built-in and `render_mode motion_vectors_transparent`: correct motion vectors for vertex-animated and transparent surfaces. With them, TAA, FSR2 and DLSS don't smear water and foam | shading language | Forward+ (on Mobile/Compatibility `PREVIOUS_FRAME` is always `false`) |
| **E2** | **NVIDIA DLSS Super Resolution / DLAA** via NVIDIA Streamline 2.10.0. Ported from [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (branch `nvidia-pt-dlss`) | `Viewport.SCALING_3D_MODE_DLSS` (= 6), `Streamline` singleton, `rendering/streamline/*` | Forward+, D3D12 and Vulkan, Windows |
| **E2b** | **NVIDIA Nsight Aftermath** (GPU crash dumps) and pass markers for PIX / Nsight / RenderDoc | command line `--debug-shaders`, `--gpu-markers`; SCons `use_aftermath` | D3D12, Vulkan |
| **E3** | **DLSS Frame Generation** (including Multi Frame Generation 3x/4x), **Reflex**, **PCL**. Our addition: HUD-less color, so the UI is not interpolated | `Viewport.frame_generation`, `RenderingServer.viewport_set_frame_generation`, `rendering/streamline/reflex_mode`, `reflex_frame_limit_us`, `frame_generation_frames` | game only (not the editor), D3D12 and Vulkan, Windows |
| **E4** | **Late opaque pass**, `render_mode opaque_late`. A material reads the screen and depth and still stays opaque: it writes depth, normals and motion vectors. Needed for water with a visible volume | shading language; `RenderSceneBuffersRD.get_opaque_late_color/depth` | Forward+ |
| **E5a** | **Ray tracing scene**: ray query and descriptor indexing enabled on Vulkan, BLAS refit, variable-length texture arrays (bindless). A per-viewport TLAS is built from meshes, MultiMesh and skinned meshes, with geometry and material data, for custom `CompositorEffect`s | `rendering/raytracing/scene_acceleration_structures`, `Viewport.raytracing_scene_*`, `RenderSceneBuffersRD.get_raytracing_*`, `RD.blas_update` | Vulkan; on D3D12 reports "unsupported" without crashing |
| **E5c** | **Path tracer** (photo mode). NVIDIA's path tracer logic (BRDF, NEE, fog) moved into one compute shader with inline ray query over the E5a TLAS. Our additions: accumulation across frames, sample limit, firefly clamp, raster sky, water as a dielectric (`rt_ior`, slope maps `rt_wave_*`, foam `rt_foam_*`), raster volumetric fog | `Environment.pathtracing_*`, `RenderingServer.environment_set_pathtracing[_accumulation]`, `Viewport.get_pathtracing_sample_count()`, `reset_pathtracing()` | Vulkan |
| **E6** | **Async compute**: a second, compute-only GPU queue. Synchronized with timeline semaphores (Vulkan 1.2) or fences (D3D12). Work that can't safely go to the second queue runs on the main queue automatically | `RD.async_compute_begin/end`, `async_compute_is_supported/is_active/set_enabled/get_info`, `TEXTURE_USAGE_ASYNC_COMPUTE_BIT`, `BUFFER_CREATION_ASYNC_COMPUTE_BIT`, `rendering/rendering_device/async_compute` | D3D12, Vulkan |
| **E7** | **Mesh and task shaders** in `RenderingDevice` (`VK_EXT_mesh_shader`): `#[task]` / `#[mesh]` sections, pipelines without vertex input | `SHADER_STAGE_TASK/MESH`, `RDShaderSource.source_task/source_mesh`, `draw_list_draw_mesh_tasks[_indirect[_count]]`, `SUPPORTS_MESH_SHADER[_INDIRECT_COUNT]`, `LIMIT_MAX_MESH_*` | Vulkan; D3D12 reports "unsupported" (SPIR-V → DXIL can't translate mesh stages); Metal: stubs |
| **E8** | **Compute on a second GPU** (usually an integrated GPU next to the discrete one): list of physical devices and a local `RenderingDevice` on any of them. Local devices bypass Streamline, Aftermath and XR | `RenderingServer.get_physical_devices()`, `RenderingServer.create_local_rendering_device_on(index)` | Vulkan, D3D12 |
| **E9** | Fixes: indirect MultiMesh command buffer leak (and its loss on reallocation); crash at exit when an async RD readback callback is a GDScript lambda | `RenderingDevice::buffer_is_valid()` (C++) | all |
| **E10** | **Directional shadow cache**: static geometry of each cascade is rendered once and reused, dynamic objects are drawn on top every frame | `GeometryInstance3D.shadow_cache_mode`, `rendering/lights_and_shadows/directional_shadow/cache*` | Forward+ |
| **E10b** | **Per-object shadow map** along the sun, fitted to the object every frame (a ship) | `DirectionalLight3D.object_shadow_layers/_size/_from_cascade` | Forward+ |
| **E11** | **Screen-space contact shadows** of the sun | `DirectionalLight3D.contact_shadow_length`, `rendering/lights_and_shadows/contact_shadows/*` | Forward+ |
| **E12** | **DDGI**: ray-traced irradiance probes. A world grid scrolls with the camera, plus a grid in the frame of a ship. Probes relocate out of walls. Sky and lights are kept in separate channels with live multipliers. An endless sea plane is hit by probe rays. Ray-tracing-only stand-ins | `Environment.ddgi_*`, `DDGIVolume3D`, `GeometryInstance3D.raytracing_mode/raytracing_opacity`, `RS.environment_set_ddgi_sea` | Vulkan + ray query |
| **E13** | **Occlusion culling of all instances** by the depth of past frames (no occluders needed), reprojected on the CPU | `Viewport.use_hiz_occlusion_culling`, `hiz_occlusion_reference`, `hiz_occlusion_reference_radius`, `rendering/occlusion_culling/use_hiz_occlusion_culling` | Forward+ |
| **E14** | Instance uniforms take only the slots they use (stock: 16 per instance, ≤ 4096 instances). Indirect MultiMesh instances are culled per shadow pass on the GPU | `RS.multimesh_set_indirect_shadow_culling` | RD renderers / Forward+ |
| **E15** | **Motion blur** by the motion vectors; **lens flare** (ghosts, halo, sun starburst) and **lens dirt** | `CameraAttributes.motion_blur_*`, `lens_flare_*`, `lens_dirt_*` | Forward+ |
| **E16** | **Parallax occlusion self-shadowing** and depth offset in BaseMaterial3D, the main light in `fragment()`. Stock fix: a shader writing `DEPTH` gets its own depth pass | `BaseMaterial3D.heightmap_self_shadow/_strength`, `heightmap_depth_offset`; `MAIN_LIGHT_DIRECTION`, `MAIN_LIGHT_SHADOW` | Forward+, Mobile |
| **E17** | **Texture mip streaming**: large textures load the mips the screen needs, within a VRAM budget | `rendering/textures/streaming/*`, `CompressedTexture2D.get_streaming_mip()` | RD renderers |
| **E18** | **GTAO with bent normals** (ported from Spartan Engine, MIT): sky and probes sampled along the bent normal, cone-based specular occlusion, multi-bounce | `Environment.ssao_gtao`, `ssao_bent_normals`, `RS.environment_set_ssao_gtao` | Forward+ |
| **E19** | **Dynamic resolution**: holds a GPU frame time by changing the 3D scale | `Viewport.dynamic_resolution_*`, `rendering/scaling_3d/dynamic_resolution/*` | all |
| **E20** | **ReSTIR DI**: many lights with ray-traced shadows and a denoiser. **E20b–d**: ray-traced shadows of the sun and the moon, including the water of E4, optionally only beyond the shadow maps | `Light3D.light_restir`, `Environment.restir_*`, `DirectionalLight3D.restir_shadow_mode` | Vulkan + ray query |
| **E21** | **Colored translucent shadows** of the sun (glass, sails) | `GeometryInstance3D.shadow_translucent`, `rendering/lights_and_shadows/directional_shadow/translucent_shadow_size` | Forward+ |
| **E22** | **Screen-space collision of GPU particles** | `GPUParticles3D.collision_screen_space` | Forward+ |
| **E23** | **Gran Turismo 7 tone mapper** (from Spartan Engine, MIT), film grain, chromatic aberration, lens distortion, AMD CAS sharpening | `Environment.TONE_MAPPER_GT7`, `CameraAttributes.image_*` | GT7: all three renderers; the rest: Forward+ |
| **E24** | **AMD FSR 3.1** upscaler and **FSR 3 frame generation** (FidelityFX SDK 1.1.4, MIT) | `Viewport.SCALING_3D_MODE_FSR3` (= 7), `Viewport.frame_generation` | upscaler: Forward+; frame generation: Vulkan with V-Sync |
| **E25** | **GPU-driven static geometry**: eligible meshes are grouped into batches. Per instance, on the GPU: frustum culling, occlusion against the Hi-Z of E13 or occluders, mesh LOD, visibility range. Stock fix: an indirect MultiMesh with LOD read past the LOD index buffer | `GeometryInstance3D.gpu_static`, `RS.scenario_get_gpu_static_info()`, `RS.multimesh_set_indirect_view_culling` | Forward+ |
| **F1** | **GTAO with a visibility bitmask** (Therrien et al. 2023): an assumed thickness of what is on screen, so thin objects don't cast dark halos | `Environment.ssao_gtao_thickness` | Forward+ |
| **F3** | **Color grading** in linear light: white balance (Kelvin, tint), shadows / midtones / highlights, ASC CDL in ACEScct, saturation; hue-preserving gamut mapping after the tone mapper | `Environment.color_grading_*`, `RS.environment_set_color_grading` | Forward+ |
| **M1–M3** | **Materials** after "Material Advances in Call of Duty: WWII" (Chan 2018): multiple-scattering diffuse, micro-shadowing of direct light by the material's AO, multi-bounce AO for every occlusion | `rendering/shading/overrides/multiscatter_diffuse`, `micro_shadowing`, `multi_bounce_ao` (on by default) | Forward+, Mobile |
| **fixes** | E6 async-compute barrier stage masks (`0013`); Compatibility 3D shaders failed to compile (`0035`); a material whose `Texture2DRD` lost its RD texture stopped drawing (`0040`) | — | — |
| **core** | Fork identity: the `mariner` feature tag in every build, default build name `mariner` | `OS.has_feature("mariner")` | all |
| **tests** | doctest coverage of all of the above, `tests/mariner/test_mariner_*.cpp`, tag `[Mariner]` | `scons tests=yes` | — |

A detailed description of each item (design, limitations, GDScript examples, manual test checklists) is in
`engine/README.md` of the game repository (in Russian). End-to-end scenes for lavapipe or a real GPU are in
`engine/tests/eNN/` and `engine/tests/f/` there.

### API in short

**DLSS and frame generation (E2, E3)**

```gdscript
var sl = Engine.get_singleton("Streamline") if Engine.has_singleton("Streamline") else null
if sl and sl.get_capability(0):                   # STREAMLINE_CAPABILITY_DLSS
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_DLSS
	get_viewport().scaling_3d_scale = 0.66        # 1.0 DLAA, 0.66 Quality, 0.58 Balanced, 0.5 Perf, 0.33 Ultra Perf
	if sl.get_capability(1):                      # STREAMLINE_CAPABILITY_DLSS_G
		get_viewport().set("frame_generation", true)
		sl.set_parameter(4, 3)                    # STREAMLINE_PARAM_DLSS_G_FRAMES: 4x (2x on RTX 40)
```

If DLSS is selected but unavailable, the engine logs a warning and falls back to bilinear scaling. Frame generation
only works together with DLSS/DLAA and only in SDR: DLSS-G doesn't support an FP16 HDR swap chain.

**Ray tracing in a CompositorEffect (E5a)**

In `_render_callback`, `RenderSceneBuffersRD` provides `has_raytracing_scene()`, `get_raytracing_tlas()`,
`get_raytracing_instance_buffer()` (128 bytes per instance), `get_raytracing_material_buffer()` (96 bytes per
instance), `get_raytracing_instance_count()`, `get_raytracing_texture_uniform_set(shader, set)` and
`get_raytracing_textures()`. Ray mask: bits 0–6 are layers 1–7, bit 7 is layers 8–20. Transparent surfaces are not
added to the TLAS. ShaderMaterial code is not run on hits: material parameters are read by name (`albedo`,
`roughness`, `metallic`, `emission`, `texture_albedo`).

**Async compute (E6)**

```gdscript
var rd := RenderingServer.get_rendering_device()
var async: bool = rd.call("async_compute_begin")   # true: the work goes to the second queue
var cl := rd.compute_list_begin()
# ... resources created with *_ASYNC_COMPUTE_BIT
rd.compute_list_end()
rd.call("async_compute_end")
```

Rule: whatever the main queue reads in frame N must have been written by the second queue in frame N−1, so use
double buffering. `async_compute_get_info()` shows what went to the second queue and why anything fell back to the
main one.

**Second GPU (E8)**

```gdscript
# on a worker thread, not the main thread
for d in RenderingServer.call("get_physical_devices"):
	print(d.index, " ", d.name, " ", d.type_name, " uma=", d.uma, " main=", d.is_main)
var rd: RenderingDevice = RenderingServer.call("create_local_rendering_device_on", 1)
# ... compute_list_* ; rd.submit(); rd.sync(); rd.buffer_get_data(buf); rd.free()
```

Data moves to the main GPU through system RAM, so keep results small: fields and spectra, not frames. Ray query and
mesh shaders are enabled on a local device only if that device supports them. Local devices don't get the E6 second
queue.

**Lighting and image (E12, E18, F1, F3)**

```gdscript
var env: Environment = $WorldEnvironment.environment
env.set("ddgi_enabled", true)             # Vulkan + ray query; otherwise ignored
env.ssao_enabled = true
env.set("ssao_gtao", true)                # E18
env.set("ssao_gtao_thickness", 0.5)       # F1, meters; 0 = horizon-based GTAO
env.set("color_grading_enabled", true)    # F3, Forward+
env.set("color_grading_temperature", 5600.0)
env.set("color_grading_shadows", Color(0.9, 0.95, 1.1))
```

**GPU-driven static geometry (E25)**

```gdscript
for mi in get_tree().get_nodes_in_group("static_props"):
	mi.set("gpu_static", true)            # eligible MeshInstance3D only; others stay on the regular path
print(RenderingServer.call("scenario_get_gpu_static_info", get_world_3d().scenario))
```

---

## Building

No special flags are needed: any build of this repository includes all the extensions and identifies itself as the
fork.

### Windows (MSVC)

Requirements: Git, Python 3 with `pip install scons`, Visual Studio 2022 Build Tools (C++).

```bat
scons platform=windows target=editor production=yes d3d12=yes vulkan=yes
scons platform=windows target=template_release production=yes d3d12=yes vulkan=yes
```

In the game repository, `engine/build_windows.bat` does all of this:
- checks the tools;
- installs the D3D12 dependencies (Mesa NIR, Agility SDK);
- downloads Streamline (`fetch_streamline.py`, verified by SHA-256);
- enables Aftermath if the SDK is placed in `engine/deps/` (`fetch_aftermath.py`);
- builds the editor and the templates.

`build_windows.bat editor` builds only the editor. `build_windows.bat tests` builds with tests and runs them.

| SCons option | default | what |
|---|---|---|
| `use_streamline` | `yes` | DLSS, DLSS-G, Reflex, PCL; turns itself off outside Windows |
| `use_aftermath` | `no` | Nsight Aftermath. Needs the SDK headers in `thirdparty/aftermath/include`; without them the build continues without Aftermath and prints a warning |

### Linux → Windows (MinGW)

```sh
scons platform=windows use_mingw=yes target=editor d3d12=yes vulkan=yes production=yes lto=none
```

Mesa NIR for gcc is expected in `bin/build_deps/mesa-x86_64-gcc`. This build is slower, has no Agility SDK, and is
meant for checking changes.

### DLLs next to the executable

Without these DLLs the engine works like stock Godot, and the DLSS capabilities report `false`.

* **Streamline 2.10.0** (production DLLs from `bin/x64/` of the
  [SDK release](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.10.0)):
  `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`. E3 also needs `sl.dlss_g.dll`,
  `nvngx_dlssg.dll`, `sl.reflex.dll`, `sl.pcl.dll` and `NvLowLatencyVk.dll`. Godot export doesn't copy them: when
  exporting a game, put them next to the executable by hand, together with `streamline.license.txt`,
  `nvngx_dlss.license.txt` and `reflex.license.txt`.
* **Aftermath**: `GFSDK_Aftermath_Lib.x64.dll`. Only needed for `--debug-shaders`.

## Tests

```sh
scons platform=<...> target=editor tests=yes
godot --test --headless --test-case="*Mariner*"
```

The tests cover:
- the shading language and compiler (E1, E4, E7, E16);
- enums, properties and their defaults, project settings, ClassDB bindings;
- the "Streamline unavailable" path;
- the fork identity;
- where Vulkan is available (including software lavapipe): device and resource creation for E6–E8 and the physical
  device list;
- data layouts and CPU-side math of the rendering features (E10–E25, F1, F3, M1–M3), e.g. the white balance matrix.

The image itself is checked by end-to-end scenes in the game repository (`engine/tests/`), not by doctest.

Regular builds are not affected by the tests.

In the game repository, `engine/test_patches.sh [--repo <clone>] [--binary <godot built with tests=yes>]` checks the
patch series in `engine/patches/`:
- the patches apply cleanly to `4.7.2-stable`;
- the result matches this fork's tree;
- `git diff --check`, clang-format on the changed lines, and the tests pass.

## Third-party code

| | source | license |
|---|---|---|
| `thirdparty/streamline/` | NVIDIA Streamline SDK 2.10.0 (headers) | MIT; DLSS / DLSS-G / Reflex: NVIDIA redistribution licenses |
| `drivers/streamline/` | ported from NVIDIA-RTX/godot, plus our changes (HUD-less, MFG, PCL) | MIT |
| `drivers/aftermath/` | ported from NVIDIA-RTX/godot; the Aftermath SDK is not part of the repository | MIT (code), NVIDIA license (SDK) |
| `servers/rendering/renderer_rd/shaders/raytracing/`, `render_raytracing*`, `render_pathtracer*` | ported from NVIDIA-RTX/godot, reworked for inline ray query | MIT |
| `shaders/effects/gtao.glsl` (E18), `shaders/effects/tonemap_gt7_inc.glsl` (E23) | ported from [Spartan Engine](https://github.com/PanosK92/SpartanEngine), MIT snapshot `cb258a150` only (later commits are under a non-MIT license) | MIT |
| `shaders/effects/cas.glsl` (E23) | AMD FidelityFX CAS, as used by Spartan Engine | MIT |
| `thirdparty/amd-fsr3/` (E24) | AMD FidelityFX SDK 1.1.4: FSR 3.1 and frame generation, with 2 small patches | MIT |

Everything else (E9–E25, F1, F3, M1–M3) is our own code, written after published papers. Ideas from other engines
(CryEngine, X-Ray, Filament, Spartan after `cb258a150`) are used without copying code. `COPYRIGHT.txt` lists every
third-party file.

## Known limitations

* Ray tracing and the path tracer are Vulkan-only: DXR is not implemented. If a game defaults to D3D12, run it
  with `--rendering-driver vulkan`. DLSS Ray Reconstruction is not ported.
* Mesh shaders are unsupported on D3D12 until Mesa's SPIR-V → DXIL translation supports mesh stages.
* DLSS-G needs an RTX 40/50, a recent driver and hardware-accelerated GPU scheduling (HAGS). It doesn't work with
  HDR (FP16 swap chain). Viewports presented to the screen on their own on top of the main one are treated by
  DLSS-G as UI.
* On D3D12 with Streamline the device is created through `sl.interposer.dll`, and the Agility SDK is not used then.
* Multi-threaded rendering (`thread_model = Multi-Threaded`) with Streamline is untested.
* Ray-traced shadow rays (E20, E20b) are opaque: alpha-tested leaves and `raytracing_opacity` stand-ins cast a full
  shadow in them.
* GPU static batches (E25) are not culled on the GPU for dual-paraboloid point light shadows and XR; they are drawn
  whole there. Fading visibility ranges and `visibility_parent` keep an instance on the regular path.
* FSR 3 frame generation is Vulkan-only and needs V-Sync.
* Color grading (F3), lens effects, motion blur, contact shadows, DDGI, ReSTIR and GPU static batches are Forward+
  only.
* GPU-dependent parts are tested by building, doctest, end-to-end scenes on lavapipe, and in the game on a PC with an
  NVIDIA RTX GPU. AMD and Intel, including integrated GPUs as the second GPU (E8), have barely been tested.

## Branches and updating

* `master` is the working branch: the 4.7.2-stable tree with all Mariner changes. The game uses the fork as a
  submodule (`engine/godot` in [mariner-simulator](https://github.com/shibisty/mariner-simulator)) pinned to a
  specific commit.
* The same changes, one patch per task, are kept in `engine/patches/` of the game repository (`git am` on top of
  `4.7.2-stable`). `engine/test_patches.sh` checks that the series produces exactly this fork's tree, except
  `README.md` and `README.godot.md`. The fork has no upstream tags; fetch the base once with
  `git fetch --no-tags https://github.com/godotengine/godot.git tag 4.7.2-stable`.
* Moving to a new Godot version: apply the patch series to the new tag, resolve conflicts, bring the result into
  `master`, then run `--test-case="*Mariner*"` and `test_patches.sh`.
