# Godot Engine — форк Mariner

Форк [Godot Engine](https://github.com/godotengine/godot) для «Mariner Simulator» и следующих наших проектов.
Основа — **Godot 4.7.2-stable**. Сверху — патчи рендера и `RenderingDevice`: DLSS, генерация кадров и Reflex,
трассировка лучей и трассировщик пути, асинхронный compute, mesh-шейдеры, вычисления на второй видеокарте.

Исходный README Godot лежит в [`README.godot.md`](README.godot.md). Лицензия та же, MIT ([`LICENSE.txt`](LICENSE.txt)).
Сторонние SDK — под своими лицензиями (см. «Сторонний код»).

**Совместимость.** Проект, написанный под стоковый Godot 4.7, открывается и работает в этом форке без изменений.
Изменения поведения действуют только там, где их включили: режимы шейдеров, настройки проекта, свойства вьюпорта или
окружения, новые методы. Новые значения перечислений добавлены в конец. Mobile и Compatibility не затронуты.

Сборка помечается `BUILD_NAME=mariner`, поэтому игра может определить форк:

```gdscript
var mariner := Engine.get_version_info().build == "mariner"
# Отдельную возможность лучше проверять напрямую — на стоковом Godot метода нет:
var has_mesh := ClassDB.class_has_method("RenderingDevice", "draw_list_draw_mesh_tasks")
```

Новые методы надо проверять через `ClassDB` / `Engine.has_singleton` и вызывать через `call()`. Тогда тот же код игры
пойдёт и на стоковом Godot.

---

## Что отличается от ванильного 4.7.2

| # | что | API (главное) | драйверы |
|---|---|---|---|
| **E1** | `PREVIOUS_FRAME` в вершинном шейдере и `render_mode motion_vectors_transparent`: правильные векторы движения у анимированных в вершинном шейдере и прозрачных поверхностей. С ними TAA, FSR2 и DLSS не смазывают воду и пену | шейдерный язык | Forward+ (на Mobile/Compatibility `PREVIOUS_FRAME` всегда `false`) |
| **E2** | **NVIDIA DLSS Super Resolution / DLAA** через NVIDIA Streamline 2.10.0. Перенесено из [NVIDIA-RTX/godot](https://github.com/NVIDIA-RTX/godot) (ветка `nvidia-pt-dlss`) | `Viewport.SCALING_3D_MODE_DLSS` (= 6), синглтон `Streamline`, `rendering/streamline/*` | Forward+, D3D12 и Vulkan, Windows |
| **E2b** | **NVIDIA Nsight Aftermath** (дампы падений GPU) и метки проходов для PIX / Nsight / RenderDoc | флаги запуска `--debug-shaders`, `--gpu-markers`; SCons `use_aftermath` | D3D12, Vulkan |
| **E3** | **DLSS Frame Generation** (включая Multi Frame Generation 3x/4x), **Reflex**, **PCL**. Своё — HUD-less цвет: интерфейс не интерполируется | `Viewport.frame_generation`, `RenderingServer.viewport_set_frame_generation`, `rendering/streamline/reflex_mode`, `reflex_frame_limit_us`, `frame_generation_frames` | только игра (не редактор), D3D12 и Vulkan, Windows |
| **E4** | **Поздний непрозрачный проход**, `render_mode opaque_late`. Материал читает экран и глубину и при этом остаётся непрозрачным: пишет глубину, нормали и векторы движения. Нужен для воды с толщей | шейдерный язык; `RenderSceneBuffersRD.get_opaque_late_color/depth` | Forward+ |
| **E5a** | **Сцена для трассировки лучей**: на Vulkan включены ray query и descriptor indexing, refit BLAS, массивы текстур переменной длины (bindless). TLAS строится на вьюпорт из мешей, MultiMesh и скиннинга, с геометрией и материалами — для своих `CompositorEffect` | `rendering/raytracing/scene_acceleration_structures`, `Viewport.raytracing_scene_*`, `RenderSceneBuffersRD.get_raytracing_*`, `RD.blas_update` | Vulkan; на D3D12 сообщает «не поддерживается» без падения |
| **E5c** | **Трассировщик пути** (фоторежим). Логика трассировщика NVIDIA (BRDF, NEE, туман) перенесена в один compute-шейдер с inline ray query по TLAS из E5a. Своё: накопление по кадрам, предел сэмплов, срез «светлячков», небо из растра, вода как диэлектрик (`rt_ior`, карты наклонов `rt_wave_*`, пена `rt_foam_*`), объёмный туман растра | `Environment.pathtracing_*`, `RenderingServer.environment_set_pathtracing[_accumulation]`, `Viewport.get_pathtracing_sample_count()`, `reset_pathtracing()` | Vulkan |
| **E6** | **Асинхронный compute**: вторая очередь GPU, только для compute. Синхронизация — timeline-семафоры (Vulkan 1.2) или fence (D3D12). Работу, которую нельзя безопасно отправить во вторую очередь, движок сам выполняет на основной | `RD.async_compute_begin/end`, `async_compute_is_supported/is_active/set_enabled/get_info`, `TEXTURE_USAGE_ASYNC_COMPUTE_BIT`, `BUFFER_CREATION_ASYNC_COMPUTE_BIT`, `rendering/rendering_device/async_compute` | D3D12, Vulkan |
| **E7** | **Mesh- и task-шейдеры** в `RenderingDevice` (`VK_EXT_mesh_shader`): секции `#[task]` / `#[mesh]`, конвейер без вершинного входа | `SHADER_STAGE_TASK/MESH`, `RDShaderSource.source_task/source_mesh`, `draw_list_draw_mesh_tasks[_indirect[_count]]`, `SUPPORTS_MESH_SHADER[_INDIRECT_COUNT]`, `LIMIT_MAX_MESH_*` | Vulkan; D3D12 — «не поддерживается» (SPIR-V → DXIL не умеет mesh-стадии); Metal — заглушки |
| **E8** | **Compute на второй видеокарте** (обычно на встроенной рядом с дискретной): список физических устройств и локальный `RenderingDevice` на любом из них. Локальные устройства обходят Streamline, Aftermath и XR | `RenderingServer.get_physical_devices()`, `RenderingServer.create_local_rendering_device_on(index)` | Vulkan, D3D12 |
| **tests** | doctest-тесты всех патчей, `tests/mariner/test_mariner_*.cpp`, группа `[Mariner]` | `scons tests=yes` | — |

Подробное описание каждого пункта — устройство, ограничения, примеры GDScript, чек-листы проверки на ПК — в
`engine/README.md` репозитория игры и в `docs/engine_plan.md`.

### Коротко по API

**DLSS и генерация кадров (E2, E3)**

```gdscript
var sl = Engine.get_singleton("Streamline") if Engine.has_singleton("Streamline") else null
if sl and sl.get_capability(0):                   # STREAMLINE_CAPABILITY_DLSS
	get_viewport().scaling_3d_mode = Viewport.SCALING_3D_MODE_DLSS
	get_viewport().scaling_3d_scale = 0.66        # 1.0 DLAA, 0.66 Quality, 0.58 Balanced, 0.5 Perf, 0.33 Ultra Perf
	if sl.get_capability(1):                      # STREAMLINE_CAPABILITY_DLSS_G
		get_viewport().set("frame_generation", true)
		sl.set_parameter(4, 3)                    # STREAMLINE_PARAM_DLSS_G_FRAMES: 4x (на RTX 40 будет 2x)
```

Если DLSS недоступен, а режим выбран, движок пишет предупреждение и масштабирует билинейно. Генерация кадров работает
только вместе с DLSS/DLAA и только в SDR: swap chain HDR в FP16 не поддерживается DLSS-G.

**Трассировка лучей в CompositorEffect (E5a)**

В `_render_callback` доступны `has_raytracing_scene()`, `get_raytracing_tlas()`,
`get_raytracing_instance_buffer()` (128 Б на экземпляр), `get_raytracing_material_buffer()` (96 Б на экземпляр),
`get_raytracing_instance_count()`, `get_raytracing_texture_uniform_set(shader, set)` и `get_raytracing_textures()`
у `RenderSceneBuffersRD`. Маска луча: биты 0–6 — слои 1–7, бит 7 — слои 8–20. Прозрачные поверхности в TLAS не
попадают. Код ShaderMaterial на попадании не выполняется: параметры материала берутся по именам `albedo`,
`roughness`, `metallic`, `emission`, `texture_albedo`.

**Асинхронный compute (E6)**

```gdscript
var rd := RenderingServer.get_rendering_device()
var async: bool = rd.call("async_compute_begin")   # true — работа пойдёт во вторую очередь
var cl := rd.compute_list_begin()
# ... ресурсы созданы с *_ASYNC_COMPUTE_BIT
rd.compute_list_end()
rd.call("async_compute_end")
```

Правило: что основная очередь читает в кадре N, вторая должна была записать в кадре N−1, то есть нужна двойная
буферизация. `async_compute_get_info()` показывает, что ушло во вторую очередь и почему что-то откатилось на основную.

**Вторая видеокарта (E8)**

```gdscript
# в рабочем потоке, не в главном
for d in RenderingServer.call("get_physical_devices"):
	print(d.index, " ", d.name, " ", d.type_name, " uma=", d.uma, " main=", d.is_main)
var rd: RenderingDevice = RenderingServer.call("create_local_rendering_device_on", 1)
# ... compute_list_* ; rd.submit(); rd.sync(); rd.buffer_get_data(buf); rd.free()
```

Обмен с основной картой идёт через ОЗУ. Результаты держать маленькими: поля и спектры, а не кадры. Ray query и
mesh-шейдеры у локального устройства включаются, только если их поддерживает само это устройство. Вторая очередь E6
у локальных устройств не создаётся.

---

## Сборка

### Windows (MSVC)

Нужны Git, Python 3 + `pip install scons` и Visual Studio 2022 Build Tools (C++).

```bat
scons platform=windows target=editor  production=yes d3d12=yes vulkan=yes use_streamline=yes BUILD_NAME=mariner
scons platform=windows target=template_release production=yes d3d12=yes vulkan=yes use_streamline=yes BUILD_NAME=mariner
```

В репозитории игры это делает `engine/build_windows.bat`. Он проверяет инструменты, ставит зависимости D3D12
(Mesa NIR, Agility SDK), скачивает Streamline (`fetch_streamline.py`, с проверкой SHA-256), подключает Aftermath, если
SDK положен в `engine/deps/` (`fetch_aftermath.py`), и собирает редактор и шаблоны. Варианты запуска:
`build_windows.bat editor` — только редактор, `build_windows.bat tests` — сборка с тестами и их прогон.

| опция SCons | по умолчанию | что |
|---|---|---|
| `use_streamline` | `yes` | DLSS, DLSS-G, Reflex, PCL; на не-Windows отключается сама |
| `use_aftermath` | `no` | Nsight Aftermath. Нужны заголовки SDK в `thirdparty/aftermath/include`; без них сборка идёт без Aftermath с предупреждением |
| `BUILD_NAME=mariner` | — | метка сборки для `Engine.get_version_info().build` |

### Linux → Windows (MinGW)

```sh
scons platform=windows use_mingw=yes target=editor d3d12=yes vulkan=yes production=yes lto=none BUILD_NAME=mariner
```

Mesa NIR для gcc нужна в `bin/build_deps/mesa-x86_64-gcc`. Этот вариант медленнее, без Agility SDK и нужен для проверки
патчей.

### DLL рядом с exe

Без этих DLL движок работает как стоковый, а возможности DLSS сообщают `false`.

* **Streamline 2.10.0** (production, `bin/x64/` из [релиза SDK](https://github.com/NVIDIA-RTX/Streamline/releases/tag/v2.10.0)):
  `sl.interposer.dll`, `sl.common.dll`, `sl.dlss.dll`, `nvngx_dlss.dll`; для E3 ещё `sl.dlss_g.dll`, `nvngx_dlssg.dll`,
  `sl.reflex.dll`, `sl.pcl.dll`, `NvLowLatencyVk.dll`. Экспорт Godot их не копирует: при экспорте игры их надо
  положить рядом с exe вручную вместе с `streamline.license.txt`, `nvngx_dlss.license.txt` и `reflex.license.txt`.
* **Aftermath**: `GFSDK_Aftermath_Lib.x64.dll`. Нужна только для `--debug-shaders`.

## Тесты

```sh
scons platform=<...> target=editor tests=yes BUILD_NAME=mariner
godot --test --headless --test-case="*Mariner*"
```

Тесты проверяют шейдерный язык и компилятор (E1, E4, E7), перечисления, свойства и значения по умолчанию, настройки
проекта, привязки ClassDB, путь «Streamline недоступен», а там, где есть Vulkan (в том числе программный lavapipe), — создание
устройств и ресурсов E6–E8 и список физических устройств. Обычные сборки тесты не затрагивают.

Серию патчей проверяет `engine/test_patches.sh --repo <клон> [--binary <godot с tests=yes>]` в репозитории игры.
Скрипт проверяет, что патчи 0001–0011 чисто ложатся на `4.7.2-stable` и совпадают с веткой, затем запускает
`git diff --check`, clang-format изменённых строк и тесты.

## Сторонний код

| | откуда | лицензия |
|---|---|---|
| `thirdparty/streamline/` | NVIDIA Streamline SDK 2.10.0 (заголовки) | MIT; DLSS / DLSS-G / Reflex — лицензии распространения NVIDIA |
| `drivers/streamline/` | перенос из NVIDIA-RTX/godot + свои доработки (HUD-less, MFG, PCL) | MIT |
| `drivers/aftermath/` | перенос из NVIDIA-RTX/godot; SDK Aftermath в репозиторий не входит | MIT (код), SDK — лицензия NVIDIA |
| `servers/rendering/renderer_rd/shaders/raytracing/`, `render_raytracing*`, `render_pathtracer*` | перенос из NVIDIA-RTX/godot с переделкой под inline ray query | MIT |

## Известные ограничения

* Трассировка лучей и трассировщик пути есть только на Vulkan: DXR не написан. Если игра по умолчанию на D3D12,
  запускать с `--rendering-driver vulkan`. DLSS Ray Reconstruction не перенесён.
* Mesh-шейдеры на D3D12 не поддерживаются, пока Mesa SPIR-V → DXIL не умеет mesh-стадии.
* DLSS-G требует RTX 40/50, свежий драйвер и включённое аппаратное планирование GPU (HAGS). Не работает с HDR
  (FP16 swap chain). Окна, которые выводятся на экран отдельно поверх главного, DLSS-G считает интерфейсом.
* На D3D12 со Streamline устройство создаётся через `sl.interposer.dll`, и Agility SDK тогда не используется.
* Многопоточный рендер (`thread_model = Multi-Threaded`) вместе со Streamline не проверялся.
* GPU-зависимые части проверены сборкой, doctest и в игре на ПК с NVIDIA RTX. На AMD и Intel, в том числе на
  встроенных GPU в роли второй карты (E8), проверки почти не было.

## Ветки и обновление

* `master` — рабочая ветка: дерево 4.7.2-stable со всеми изменениями Mariner. Игра подключает форк сабмодулем
  (`engine/godot` в репозитории [mariner-simulator](https://github.com/shibisty/mariner-simulator)) и закрепляет
  конкретный коммит.
* Те же изменения по одному патчу на задачу лежат в `engine/patches/` репозитория игры (`git am` поверх
  `4.7.2-stable`). `engine/test_patches.sh` проверяет, что серия патчей даёт ровно дерево форка (кроме `README.md` и
  `README.godot.md`). Тега апстрима в форке нет, его нужно один раз подтянуть:
  `git fetch --no-tags https://github.com/godotengine/godot.git tag 4.7.2-stable`.
* Переход на новую версию Godot: наложить серию патчей на новый тег, поправить конфликты, перенести результат в
  `master` и прогнать `--test-case="*Mariner*"` и `test_patches.sh`.
