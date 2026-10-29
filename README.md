# Godot Engine — Mariner fork

A fork of [Godot Engine](https://github.com/godotengine/godot) for *Mariner Simulator* and our future projects.
It is based on **Godot 4.7.2-stable** and adds rendering and `RenderingDevice` extensions: DLSS, frame generation
and Reflex, ray tracing and a path tracer, async compute, mesh shaders, and compute on a second GPU.

The original Godot README is in [`README.godot.md`](README.godot.md). The license is the same, MIT
([`LICENSE.txt`](LICENSE.txt)). Third-party SDKs keep their own licenses (see "Third-party code").

**Compatibility.** A project written for stock Godot 4.7 opens and runs in this fork unchanged. New behaviour only
applies where it is enabled: shader render modes, project settings, viewport and environment properties, new
methods. New enum values are appended at the end. The Mobile and Compatibility renderers are not affected.

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
| **core** | Fork identity: the `mariner` feature tag in every build, default build name `mariner` | `OS.has_feature("mariner")` | all |
| **tests** | doctest coverage of all of the above, `tests/mariner/test_mariner_*.cpp`, tag `[Mariner]` | `scons tests=yes` | — |

A detailed description of each item (design, limitations, GDScript examples, manual test checklists) is in
`engine/README.md` and `docs/engine_plan.md` of the game repository (in Russian).

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
- the shading language and compiler (E1, E4, E7);
- enums, properties and their defaults, project settings, ClassDB bindings;
- the "Streamline unavailable" path;
- the fork identity;
- where Vulkan is available (including software lavapipe): device and resource creation for E6–E8 and the physical
  device list.

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

## Known limitations

* Ray tracing and the path tracer are Vulkan-only: DXR is not implemented. If a game defaults to D3D12, run it
  with `--rendering-driver vulkan`. DLSS Ray Reconstruction is not ported.
* Mesh shaders are unsupported on D3D12 until Mesa's SPIR-V → DXIL translation supports mesh stages.
* DLSS-G needs an RTX 40/50, a recent driver and hardware-accelerated GPU scheduling (HAGS). It doesn't work with
  HDR (FP16 swap chain). Viewports presented to the screen on their own on top of the main one are treated by
  DLSS-G as UI.
* On D3D12 with Streamline the device is created through `sl.interposer.dll`, and the Agility SDK is not used then.
* Multi-threaded rendering (`thread_model = Multi-Threaded`) with Streamline is untested.
* GPU-dependent parts are tested by building, doctest, and in the game on a PC with an NVIDIA RTX GPU. AMD and Intel,
  including integrated GPUs as the second GPU (E8), have barely been tested.

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
