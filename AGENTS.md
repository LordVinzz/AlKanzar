# AGENTS.md — AlKanzar repository guide

This file is the operational entry point for an LLM or a new contributor.
Read it before changing code. It describes where responsibilities live, how
the runtime is wired, which dependencies are allowed, and how to validate a
change. More focused design details live in `ARCHITECTURE.md`; requirements and
delivery history live in `TODO.md`.

## 1. Project in one minute

AlKanzar is a C++20 isometric CRPG/engine prototype inspired by the experience
of classic real-time-with-pause CRPGs. It uses SDL2, OpenGL, GLM, spdlog,
Dear ImGui and CMake. It currently provides:

- a direct/deferred renderer, shadows, glTF assets and editor overlays;
- an ECS with scene creation, transforms, lighting, animation and physics;
- navmesh generation, Polyanya pathfinding and asynchronous path requests;
- a fixed-step simulation loop with pause and speed control;
- player, friendly NPC and hostile NPC character profiles;
- pure character rules, derived statistics and an ImGui character inspector;
- green, blue and red ground indicators for player, friendly and hostile
  characters respectively;
- architecture, unit, integration and source-size checks through CTest.

NPC AI is intentionally not implemented yet. Do not infer that friendly or
hostile affiliation implies autonomous behavior.

The long-term product and all pending work are described in the requirements
matrix in `TODO.md`. The named RPG rules and formulas come from
`systeme_regles_jdr_jeu_video_v1.1.docx`; consult that document before changing
the RPG domain. Do not silently replace its terminology or invent missing
formulas.

## 2. First steps for any task

1. Read the relevant requirement and its refinement history in `TODO.md`.
2. Locate the responsibility using the map and routing table below.
3. Read the public header first, then its implementation and existing tests.
4. Check `ARCHITECTURE.md` before adding an include or moving a type.
5. Search for every use of the symbol with `rg`; many features cross scene
   creation, ECS lifecycle, editor descriptors and frame extraction.
6. Make the smallest coherent change at the correct layer.
7. Add or update focused tests, then build and run all CTest tests.
8. Update `TODO.md` when an exigence is introduced, refined or delivered.

Useful discovery commands:

```bash
rg --files src tests
rg -n "SymbolOrConcept" src tests CMakeLists.txt
rg -n "ComponentName" src/core/ecs src/core/editor src/core/scene tests
git status --short
git diff --check
```

Preserve unrelated work in a dirty worktree. In particular, `imgui.ini` is a
local editor-session artifact that is ignored but may still be tracked in old
clones; do not include its incidental changes in a commit.

## 3. Repository map

### Root files

| Path | Purpose |
|---|---|
| `CMakeLists.txt` | Defines content, rules, core, executable and test targets. Every new owned `.cpp` must be listed in the appropriate target. |
| `ARCHITECTURE.md` | Canonical dependency direction and layer contracts. |
| `TODO.md` | Requirements matrix, iteration history and delivered/pending scope. |
| `README.MD` | Dependency installation, basic build and Doxygen commands. |
| `systeme_regles_jdr_jeu_video_v1.1.docx` | Game-design source for character and RPG rules. |
| `Doxyfile` | Doxygen configuration; generated output goes to ignored `docs/`. |
| `assets/navmeshes/` | Authored/versioned navmesh assets copied into the build tree. |
| `assets/scenes/` | Human-readable versioned Lua scene assets copied into the build tree. |
| `third_party/` | Vendored implementation code such as Polyanya and CDT. Avoid editing unless the task explicitly targets it. |
| `tests/` | Assert-based test executables and CMake quality checks. |

### `src/core`

| Directory | Responsibility and starting points |
|---|---|
| `app/` | Process orchestration, modes, input translation, fixed-step loop and shared services. Start with `Application.cpp`, `ApplicationInput.cpp`, `AppState.cpp`, `EngineServices.hpp`, `SimulationClock.hpp` and `TimeContext.hpp`. |
| `content/` | Serializable/configurable RPG data and enums with no engine behavior. Start with `CharacterData.hpp`. |
| `rules/` | Pure deterministic domain calculations and normalization. Start with `CharacterRules.hpp/.cpp`. This target compiles independently as `alkanzar_rules`. |
| `simulation/` | Mutable runtime components and adapters between ECS state and pure rules. Start with `CharacterComponents.hpp` and `CharacterSimulation.hpp/.cpp`. |
| `ecs/` | Entity identity, pools, component stores, world ownership and technical ECS components. `World.hpp` owns all stores and lifecycle cleanup. |
| `presentation/` | Lightweight presentation contracts shared without importing full editor/render APIs. `ComponentKind.hpp` is the current example. |
| `editor/` | ImGui windows, inspectors, component descriptors, selection, editor session state and undo/redo commands. |
| `events/` | Synchronous signals and queued application events. `Events.hpp` defines the `AppEvent` variant. |
| `scene/` | SCN Lua loading/validation, declarative `SceneBlueprint`, asset/entity construction and camera math. Start with `SceneAsset.hpp`, `SceneRegistry.cpp` and `SceneFactory.hpp`. |
| `navigation/` | Navmesh data, authoring, serialization, baking, runtime queries, Polyanya, async requests, agent motion and editor integration. See the navigation map below before editing. |
| `animation/` | Runtime animation pose, clip and blend updates. |
| `physics/` | Physics-system updates and collider/rigidbody behavior. |
| `transform/` | Local/world transform propagation and transform caches. |
| `lighting/` | Runtime light updates, light volumes and material-library support. |
| `systems/` | Cross-cutting engine systems: picking, render extraction and task scheduling. |
| `profiling/` | CPU/GPU/runtime profiling, Perfetto capture and trace export. |

### `src/render`

The render library uses namespace `render` and is a presentation-only
consumer of extracted frame data.

| Directory | Responsibility |
|---|---|
| `engine/` | SDL/OpenGL renderer lifetime, render paths, frame options, scene views, occlusion and GPU resource registry. `RenderEngine.hpp` is the public facade. |
| `pipeline/` | Geometry, lighting, shadows and editor/debug overlay passes. Ground-indicator drawing belongs here, not in rules or simulation. |
| `resources/` | Meshes, materials, shaders, textures, GL buffers and glTF loading/processing. |
| `shaders/` | GLSL sources copied to `build/shaders`. Add new shader paths explicitly to `src/render/CMakeLists.txt`. |
| `models/` | Versioned glTF/GLB source assets copied to `build/models`; the model list is explicit in CMake. |
| `textures/` | Texture source assets copied recursively to `build/textures`. |

`src/main.cpp` configures logging and SDL, creates `core::Application`, then
hands over control to the application loop.

### CMake target graph

Do not infer layer boundaries only from the broad `alkanzar_core` target. The
important target relationships are:

```text
alkanzar_lua (Lua 5.5 core + auxiliary API, no standard libraries)
            └── alkanzar_core (PRIVATE)

alkanzar_content (INTERFACE headers)
    └── alkanzar_rules (STATIC, pure domain rules)
            └── alkanzar_core (STATIC)

imgui + SDL2 + OpenGL + GLM
    └── alkanzar_render (STATIC)
            └── alkanzar_core
                    └── AlKanzar (executable)
```

`alkanzar_core` also links spdlog and Threads. Dear ImGui is pinned in
`src/render/CMakeLists.txt` and fetched by CMake. Keep `alkanzar_rules`
independently linkable: a pure-rules test must not acquire SDL, ImGui or render
dependencies transitively.

### Current limits: do not assume these exist

The prototype has engine foundations, not a completed CRPG. At the time this
guide was written:

- the default scene is loaded from the SCN V1 Lua format, but other zone,
  campaign and save-game content formats are not implemented yet;
- Gameplay, Editor and TestTool modes are isolated through explicit capability
  policies; broader product-level test tooling remains a requirement;
- character inspection and formulas exist, but combat, abilities, inventory,
  dialogue, quests, campaign state, save/load and NPC AI are not complete;
- navigation/pathfinding exists, but party formations and general combat
  orders are not implemented;
- the full asynchronous resource manager, content validators and authoring
  tools remain pending;
- there is no permission to copy Baldur's Gate content, rules text, assets or
  protected names; all shipped content must be original.

Confirm the live state in `TODO.md` instead of relying only on this summary.

## 4. Architecture and allowed dependencies

The mandatory gameplay dependency flow is:

```text
content -> rules -> simulation -> presentation/UI
```

A layer may depend only on layers to its left. The important contracts are:

- `core/content` contains passive data, identifiers, enums and pure
  engine-independent format primitives. It must not include rules, ECS,
  systems, app, SDL, OpenGL, ImGui or renderer code.
- `core/rules` accepts explicit content values and returns values. It must not
  access `World`, global time, random global state, simulation components,
  editor APIs or rendering APIs.
- `core/simulation` may adapt runtime state to rule inputs and mutate runtime
  state. It must not depend on editor, renderer, SDL, OpenGL or ImGui.
- `core/ecs` is infrastructure. It must not know the editor or ImGui. Some
  existing technical rendering components remain there as presentation
  adapters, but they must never leak into pure rules.
- editor and renderer may consume simulation results. Presentation requests a
  mutation through a command/event or an explicit simulation API; rendering
  must not become the owner of gameplay state.
- communicate from world state to rendering through `FrameSceneData` in
  `core/app/FrameData.hpp`, populated by extraction/sync systems.

CTest `alkanzar_architecture_layers` performs literal source scans for forbidden
dependencies. It also sees tokens in comments and strings, so do not mention a
forbidden include path inside a lower layer merely as documentation. When the
architecture evolves deliberately, update `ARCHITECTURE.md`, CMake targets and
the architecture test together.

### Correct placement examples

- Race, kit, skill and serializable values: `core/content`.
- XP thresholds or derived-stat formulas: `core/rules`.
- Affiliation, current target, orders and active runtime effects:
  `core/simulation`.
- ECS storage and entity lifetime: `core/ecs`.
- ImGui editing and formula display: `core/editor`.
- Ground rings or selection outlines: frame extraction plus `src/render`.

## 5. Runtime flow

The main frame is orchestrated in `Application::run()`:

1. SDL events are pumped and translated to `AppEvent` values.
2. The event bus dispatches queued events and mode transitions.
3. `SimulationClock` clamps unsafe frame deltas and fills a fixed-step
   accumulator.
4. ImGui begins a new presentation frame and free-camera input is sampled.
5. For every available fixed step, state, navigation, animation, physics,
   transforms, lighting and frame extraction are updated.
6. State UI is drawn once per rendered frame, not once per simulation tick.
7. navigation debug data is synchronized into `FrameSceneData`.
8. the renderer consumes the immutable frame snapshot, draws ImGui and
   presents.
9. profiling results are collected and the frame limiter runs.

Simulation behavior must use `TimeContext::deltaSeconds`, which is the fixed
step. Do not use wall-clock time or `frameDeltaSeconds` for gameplay. Rendering
interpolation may use `interpolationAlpha`. Pausing stops fixed updates but does
not stop event handling, UI or rendering.

`SimulationClock` currently defaults to 60 Hz, clamps a real frame to 250 ms
and allows at most eight accumulated steps per rendered frame. Preserve these
safety properties when changing timing code.

Application modes are `Bootstrap`, `Gameplay`, `Editor`, `TestTool` and
`Shutdown`, defined in `AppMode.hpp`. Their fixed-step systems, accepted input
and presentation capabilities are centralized there. Shared owned services
live in `EngineServices.hpp`. A new
long-lived subsystem normally needs:

1. a focused public type;
2. an owned instance in `EngineServices`;
3. initialization at bootstrap or construction;
4. update at the correct fixed-step or render-frame point;
5. focused tests and profiling if it is frame-critical.

## 6. ECS and character model

Entities are generational `EntityId` values. `World` owns an `EntityPool`, one
`ComponentStore<T>` per component type and runtime caches. `World` is not a
thread-safe database.

When adding a component, audit every one of these places:

1. Define passive domain data in `core/content` or runtime data in
   `core/simulation`; keep only technical engine components in
   `core/ecs/Components.hpp`.
2. Add the `ComponentStore<T>` to `World`.
3. Remove it in both `World::destroyEntity()` and `World::clear()`.
4. Add a `ComponentKind` only if selection/editor presentation needs it.
5. Register its descriptor in `core/editor/ComponentDescriptors.cpp` or a
   focused descriptor file.
6. Make editor mutations undoable through `CommandHistory` when user-facing.
7. Extend `SceneBlueprint`, `SceneRegistry` and `SceneFactory` if scenes can
   author or spawn it.
8. Extract presentation data into `FrameSceneData` if rendering needs it.
9. Add lifecycle, scene and extraction tests.

Characters currently use a bundle of:

- `CharacterComponent` for affiliation, race, kit, XP and ground-ring radius;
- `AbilityScoresComponent`;
- `SkillRanksComponent`;
- `CharacterVitalsComponent`.

`World::characterOwnerEntity()` resolves picked glTF child sections back to
the character root. Preserve this behavior when changing scene hierarchies,
selection or skinned-model ownership.

The default scene defines three demonstrators in `SceneRegistry.cpp`: player,
friendly NPC and hostile NPC. `SceneFactory.cpp` turns blueprints into ECS
entities and render resources. NPCs must remain uncontrolled until an explicit
AI requirement is implemented.

## 7. Rules workflow

For a new or changed game rule:

1. Verify the design in `systeme_regles_jdr_jeu_video_v1.1.docx` and the
   relevant `TODO.md` requirement.
2. Put serializable inputs/enums in `core/content`.
3. Expose a small value-oriented function in `core/rules`.
4. Keep the implementation deterministic and independent from engine state.
5. Test boundary values and representative profiles in
   `tests/rules_layer_tests.cpp` when only `alkanzar_rules` is needed.
6. Add broader component/lifecycle integration coverage in
   `tests/character_rules_tests.cpp` when simulation or ECS is involved.
7. Adapt from runtime components in `core/simulation`; do not make rules accept
   `World` or engine components for convenience.
8. Show derived results in the editor without duplicating formulas there.

Historical maximum HP is intentional: normalization clamps current values but
does not retroactively recalculate earned maximum HP after a race, ability or
kit edit. Treat changes to that invariant as game-design changes, not cleanup.

## 8. Editor, selection and undo/redo

ImGui code belongs under `core/editor`. Major entry points are:

- `EditorUiMainWindow.cpp`: main editor menu/window;
- `EditorUiHierarchy.cpp`: scene hierarchy and selection;
- `EditorUiInspector.cpp`: selected-entity inspector shell;
- `ComponentRegistry.*` and `ComponentDescriptors.cpp`: component tabs and
  add/remove behavior;
- `CharacterInspector.cpp`: editable/derived character statistics;
- `EditorUiNavigation.cpp`: navmesh authoring and test controls;
- `EditorUiProfiler.cpp`: profiling UI;
- `EditorSession.hpp`: persisted window/tool state;
- `SelectionModel.hpp`: editor-only entity/component selection state;
- `PartySelectionModel.hpp`: ordered runtime party selection and leader used
  by gameplay orders independently from editor selection;
- `PartySelectionSystem.hpp/.cpp`: screen-space drag selection restricted to
  player-controlled character profiles and per-frame selected/deselected ring
  presentation.

User-visible edits should normally be represented by `ICommand` or
`SnapshotCommand<T>` and executed through `CommandHistory`. Supply stable,
specific merge keys for continuous widgets so dragging can merge related edits
without merging unrelated entities or fields. The apply/undo callback must
also publish relevant dirty/change events.

Use stable ImGui IDs (`PushID`, `##hidden-id`, entity/component identity) for
repeated controls. Always pair `Begin`/`End`, `BeginTable`/`EndTable`,
`TreeNode`/`TreePop`, `PushID`/`PopID`, including early-return paths.

## 9. Navigation map and concurrency rules

Navigation is intentionally split into small files because every owned source
must stay below 500 lines. Start at `Navigation.hpp` and route by concern:

- asset lifecycle/serialization: `NavigationAssetLifecycle.cpp`,
  `NavigationAssetSerialization.cpp`;
- editor actions: `NavigationEditor.cpp`, `NavigationEditorSupport.cpp`;
- geometry/tag extraction and bake: `NavigationBakeSources.cpp`,
  `NavigationGenerator.cpp`, `NavigationDelaunay*.cpp`,
  `NavigationBakeRuntimeCells.cpp`;
- runtime construction and queries: `NavigationRuntimeBuilder.cpp`,
  `NavigationRuntimeQueries.cpp`, `NavigationRuntimeSupport.cpp`;
- endpoint/visibility/corridor/funnel logic: files with those terms;
- exact pathfinding: `Polyanya*.cpp` and vendored `third_party/polyanya`;
- async requests/results: `NavigationAgentRequests.cpp`,
  `NavigationAgentResults.cpp`;
- movement: `NavigationAgentMotion.cpp`;
- render/debug snapshot: `NavigationFrameSync.cpp`.

Path workers operate on immutable `NavigationSolveSnapshot` data. They must not
read or mutate `World` concurrently. Results are revisioned and applied back on
the main thread; stale or cancelled results must be discarded. The task
scheduler is also used for frame-bound parallel work, which must join before
the next dependent phase or presentation.

Do not capture references to short-lived frame data in an async task. Make
ownership, cancellation and result-application thread explicit. Profile
expensive navigation changes and test unreachable, boundary and stale-result
cases.

### SCN V1 scene assets

Scene assets begin with the visible 10-byte header `V1SCN-----`, followed by a
newline and restricted Lua. `assets/scenes/DefaultScene.scene` is the canonical
example. Author objects with `Create({...})`, configure them through captured
methods such as `object.transform(...)`, add every object exactly once with
`scene.add(object)`, and finish with `scene.build()`.

Start scene-format changes in `SceneAsset.hpp/.cpp`. Field/type validation is
split across `SceneAssetLuaFields.cpp`, `SceneAssetCharacter.cpp`,
`SceneAssetLights.cpp` and `SceneAssetParser.cpp`. `SceneRegistry.cpp` resolves
the staged asset, while `SceneFactory.cpp` is only responsible for converting
the resulting `SceneBlueprint` into ECS/render resources.

Do not call `luaL_openlibs`, expose file/network/process APIs, accept binary Lua
chunks, or let scene scripts access `World` or the renderer. Keep memory and
instruction budgets active. Add new SCN fields to the strict allowlists and
tests; a misspelled field must fail instead of silently taking a default.
Keep model and navmesh references portable and relative; SCN validation rejects
absolute paths and `..` traversal.

## 10. Rendering and assets

The renderer must consume extracted snapshots, not query the live ECS. To add a
new visible gameplay feature:

1. define a compact `Frame*` presentation structure in `FrameData.hpp`;
2. populate it in `RenderExtractionSystem` or a focused sync system;
3. consume it in the appropriate renderer/pipeline pass;
4. support both direct and deferred paths when the feature should appear in
   both;
5. add a CPU-side extraction/math test where possible;
6. visually verify the feature in the running editor/game.

OpenGL objects are owned by render resources and should be created, used and
destroyed on the thread with the active GL context. Prefer RAII wrappers and
the existing `RenderResourceRegistry`. Do not issue OpenGL calls from pure
rules, ECS components or worker tasks.

Asset-copy behavior is defined in `src/render/CMakeLists.txt`:

- shaders and models are explicit lists: update CMake when adding one;
- textures, navmeshes and scenes are discovered and copied as directories;
- runtime assets are staged under `build/shaders`, `build/models`,
  `build/textures`, `build/navmeshes` and `build/scenes`.

Versioned content files reserve exactly 10 bytes for their header. Use the
shared codec in `core/content/ContentFileHeader.hpp`; do not hand-roll another
magic prefix. The byte layout is `V<decimal version><uppercase type>` followed
by validated padding: zero bytes for binary formats, visible `-` bytes for
text formats. NAV V1 is `V1NAV\0\0\0\0\0`; SCN V1 is `V1SCN-----`. Open files
in binary mode, validate the type before parsing the payload, and keep the type
registry in `ARCHITECTURE.md` current. Compatibility with a legacy payload
belongs in its format-specific reader.

Run the executable with the build directory as its working directory so these
relative assets resolve consistently:

```bash
cd build
./AlKanzar
```

## 11. Events, state changes and dirty flags

SDL events are translated in `ApplicationInput.cpp`; domain/engine handlers
subscribe in `Application::bindEventHandlers()`. Prefer typed events from
`Events.hpp` over coupling an input key directly to a subsystem.

When adding an event:

1. define its payload in `Events.hpp`;
2. add it to the `AppEvent` variant;
3. publish it from input/editor/system code;
4. subscribe at the owner of the state mutation;
5. avoid retaining references to event payloads after dispatch.

Transform and light changes have explicit dirty signals and caches. Mutating a
transform or light without marking/publishing the appropriate change can
produce stale world matrices, bounds or lighting. Follow nearby editor command
code rather than writing directly to a component from a widget.

## 12. Code style

There is currently no repository `.clang-format` or `.clang-tidy`; adjacent
code is the source of truth. Preserve these established conventions:

- C++20, no compiler extensions.
- Four spaces for indentation; no tabs.
- Opening braces on the same line for namespaces, types, functions and control
  flow.
- `PascalCase` for classes, structs, enums and aliases.
- `camelCase` for functions, methods and local variables.
- `snake_case_` is not used; private data members use `camelCase_` with a
  trailing underscore.
- Constants use `kPascalCase`; enum values use `PascalCase`.
- Namespace names are lowercase (`core`, `render`, focused detail namespaces).
- Headers use `#pragma once` unless platform GL guards require additional
  macros.
- Prefer one public header plus focused `.cpp` files split by responsibility.
- Use `const`, references and `[[nodiscard]]` on queries/value-returning
  functions where ignoring the result is likely a bug.
- Use brace initialization, explicit default member values, `nullptr`,
  `std::optional` for absence and smart pointers for ownership.
- Prefer scoped `enum class` values and fixed-width integers where persistence
  or cross-thread identity matters.
- Use early returns to reduce nesting.
- Close namespaces with comments such as `}  // namespace core`.
- Use `spdlog` instead of `printf`/`std::cout` in application code.
- Add `ALKANZAR_PROFILE_SCOPE` around meaningful frame-critical work, but do
  not flood tiny leaf functions with scopes.
- Keep comments focused on invariants, ownership, units, algorithms or reasons;
  do not narrate obvious syntax.

Include order is not fully automated. Follow the local file pattern: own header
first in `.cpp`, then standard library, third-party and project headers in
readable groups. Avoid broad umbrella includes and avoid moving dependencies
into headers when a forward declaration is sufficient.

All project-owned files under `src` must contain strictly fewer than 500 lines.
CTest `alkanzar_src_file_length` enforces this. Split files along business or
pipeline boundaries before approaching the limit; do not compress formatting
or combine unrelated logic to evade it.

## 13. Build, test and documentation commands

From the repository root:

```bash
cmake -S . -B build -DBUILD_TESTING=ON
cmake --build build --parallel
ctest --test-dir build --output-on-failure
```

The first configure may download Dear ImGui through CMake FetchContent and
therefore may require network access. Required system packages are documented
in `README.MD`.

Focused checks:

```bash
ctest --test-dir build -R alkanzar_rules_layer_tests --output-on-failure
ctest --test-dir build -R alkanzar_character_rules_tests --output-on-failure
ctest --test-dir build -R alkanzar_architecture_layers --output-on-failure
ctest --test-dir build -R alkanzar_src_file_length --output-on-failure
cmake --build build --target AlKanzar --parallel
```

CTest currently registers:

| Test | Scope |
|---|---|
| `alkanzar_core_tests` | Engine/ECS/navigation/render-extraction integration and math behavior. |
| `alkanzar_character_rules_tests` | Character rules integrated with ECS, scenes, descriptors, selection and indicators. |
| `alkanzar_rules_layer_tests` | Pure rules linked without the engine/render target. |
| `alkanzar_app_mode_tests` | Mode capabilities/transitions, selection isolation, CLI launch mode and deterministic test scene. |
| `alkanzar_architecture_layers` | Forbidden dependency scan. |
| `alkanzar_src_file_length` | Strict `< 500` line limit for owned source files. |

Tests use standard `assert`, not a third-party test framework. Add a focused
test function, call it from the test executable's `main`, and use deterministic
inputs. A disabled assertion build would invalidate these tests, so do not add
`NDEBUG` to test targets.

Generate API documentation with:

```bash
doxygen Doxyfile
```

The generated `docs/`, `build/`, logs, captures and root
`compile_commands.json` are ignored outputs and should not be committed.

## 14. Runtime controls and diagnostics

Current controls relevant to testing:

| Input | Action |
|---|---|
| `E` or `F1` | Toggle Editor and the previous runtime mode (Gameplay or TestTool). |
| `Space` | Pause/resume fixed-step simulation in Gameplay or Editor. |
| `-`, `=` or keypad `+` | Cycle Gameplay/Editor simulation speed through 0.5×, 1×, 2× and 4×. |
| Left click | Select a controlled character or move the current leader in Gameplay; select/test navigation in Editor depending on tool mode. |
| Left-drag | Draw a green Gameplay marquee and replace the party selection; releasing over no controlled character clears it. |
| Middle-drag / wheel | Pan / zoom the isometric camera. |
| `C` | Toggle free camera; right-drag looks around while enabled. |
| `0`–`8` | Select final/deferred debug views. |
| `[` / `]` | Step shadow debug cascades. |
| Ctrl/Cmd+Z, Ctrl/Cmd+Shift+Z or Ctrl/Cmd+Y | Undo/redo editor commands. |
| Ctrl/Cmd+I/P/N/S | Toggle inspector, profiler, navmesh or hierarchy windows in Editor. |
| `Esc` | Quit. |

Startup modes can be selected explicitly:

```bash
./AlKanzar --gameplay
./AlKanzar --editor
./AlKanzar --test-tool
```

`--test-tool` loads the deterministic three-character test scene, runs the
fixed-step runtime systems and exposes neither editor UI nor gameplay orders.

Useful environment variables found in the runtime:

- `ALKANZAR_FLUSH_INFO_LOGS=1`: flush info logs immediately;
- `ALKANZAR_DISABLE_PROFILER_FRAME=1`: disable per-frame profiler capture;
- `ALKANZAR_DISABLE_SCHEDULER_PROFILING=1`: detach scheduler profiling;
- `ALKANZAR_LOG_FRAME_STAGES=1`: log frame-stage boundaries;
- `ALKANZAR_LOG_FRAME_STAGE_LIMIT=<n>`: limit those diagnostic frames.

The main log is `AlKanzar.log` in the process working directory. Performance
captures can be exported from the profiler UI for Perfetto analysis.

## 15. Problem-to-file routing

| Problem | Inspect first | Then inspect |
|---|---|---|
| Startup, shutdown or mode transition | `src/main.cpp`, `app/Application.cpp`, `app/AppState.cpp` | `EngineServices.hpp`, renderer initialization, logs |
| Pause, speed or delta-time issue | `SimulationClock.hpp`, `TimeContext.hpp` | fixed-step section of `Application.cpp`, input events |
| Incorrect character statistic | rules DOCX, `CharacterRules.*` | `CharacterData.hpp`, rules tests, inspector display |
| Character data missing/stale | `CharacterComponents.hpp`, `World.hpp` | scene blueprint/factory, component descriptor, normalization |
| Wrong NPC/player ring | `RenderExtractionSystem*`, `FrameGroundIndicator` | overlay renderer direct/deferred paths, affiliation data |
| Picking selects a mesh child | `PickingSystem.*`, `World::characterOwnerEntity()` | `SelectionModel`, scene/skinned hierarchy |
| Gameplay marquee selection issue | `ApplicationPartySelection.cpp`, `PartySelectionSystem.*` | `PartySelectionModel.hpp`, `FramePartySelectionMarquee`, `SceneOverlayMarquee.cpp` |
| Click-to-move/path issue | `ApplicationInput.cpp`, `Navigation.hpp` | request/result, hit-test, Polyanya/corridor/funnel files |
| Navmesh bake issue | `NavigationBakeSources.cpp`, `NavigationGenerator.cpp` | Delaunay/cell/coverage files and editor UI |
| Scene syntax/load issue | `SceneAsset.cpp`, `assets/scenes/DefaultScene.scene` | field parser files, `SceneRegistry.cpp`, scene-asset tests |
| Transform or bounds stale | `TransformSystem.*`, `World` dirty flags | editor commands, render extraction |
| Light/shadow issue | `LightSystem.*`, render light/shadow pipeline | frame extraction, materials, shaders |
| Animation/skinning issue | `AnimationSystem.*`, animation components | glTF processing, `RenderExtractionAnimation.*`, shader skinning |
| ImGui panel/edit issue | relevant `EditorUi*.cpp`/inspector | `EditorSession`, `CommandHistory`, component registry |
| Rendering artifact | `RenderEngine`, active render path | pipeline pass, GL resource wrapper, shader |
| Performance regression | profiler UI/capture, `ProfilerService` | task scheduler, runtime policy, relevant profiled subsystem |
| CMake/link error after adding a file | root or render `CMakeLists.txt` | target dependency direction and architecture test |
| Asset missing at runtime | `src/render/CMakeLists.txt` copy rules | process working directory and source asset path |

## 16. Updating requirements and Git history

`TODO.md` is a traceability matrix, not a checkbox list. Maintain it as follows:

- one row per requirement;
- give refinements their own stable hierarchical IDs;
- keep the parent row and write `R→child IDs` in the refining iteration;
- use `I` for introduction, `L` for delivery and `I+L` for both;
- add a new iteration column for a meaningful delivery rather than rewriting
  previous history;
- keep exit criteria and the definition of done as requirements;
- do not mark a parent delivered when only some mandatory children are done.

When asked to commit, use a concise English Conventional Commit/Gitflow-style
subject such as `feat(rules): add damage resistance resolution` or
`fix(navigation): discard stale path results`. Stage only files belonging to
the requested change, report tests run, and never include incidental generated
or editor-session files.

## 17. Definition of done for code changes

A change is ready only when all applicable items are true:

- responsibility is placed in the correct architectural layer;
- dependency direction is preserved;
- ECS lifecycle, scene creation and editor descriptors are complete where
  applicable;
- fixed-step versus render-frame timing is correct;
- worker tasks do not access mutable ECS or GL state unsafely;
- user-facing editor mutations support undo/redo where appropriate;
- direct and deferred rendering paths remain consistent where applicable;
- focused regression tests cover normal and boundary behavior;
- `cmake --build build --parallel` succeeds;
- `ctest --test-dir build --output-on-failure` succeeds;
- `git diff --check` succeeds;
- no owned source reaches 500 lines;
- `TODO.md`, `ARCHITECTURE.md` and `README.MD` are updated if their contracts or
  requirements changed;
- unrelated local changes and generated artifacts remain untouched.

When uncertain, favor explicit data flow, pure rules, deterministic fixed-step
simulation, snapshot-based presentation and a focused test over a shortcut
that crosses layers.
