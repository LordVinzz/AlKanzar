# Build

```bash
cmake -S . -B build
cmake --build build
```

Run from the build directory so staged assets resolve next to the executable:

```bash
cd build
./AlKanzar --gameplay
```

Available startup modes are `--gameplay` (default), `--editor`, and
`--test-tool`. The test-tool mode loads a deterministic scene without editor
UI or gameplay input.

# Scene assets

The default scene is authored in readable Lua at
`assets/scenes/DefaultScene.scene`. Its first line is the fixed 10-byte header
`V2SCN-----`; the remaining lines use the restricted scene DSL:

```lua
scene = Create({ type = "Scene" })
ground = Create({
    type = "Primitive", id = "ground", name = "Ground",
    shape = "Plane", material = "Soil", layer = "Ground",
})
ground.transform({ scale = { x = 1000, y = 1, z = 1000 } })
scene.add(ground)
object = Create({ type = "Model", id = "object", name = "Object", asset = "Object.glb" })
object.transform({ position = { x = 0, y = 0, z = 0 } })
scene.add(object)
scene.build()
```

Every SCN V2 object has a stable unique `id`. Transformable objects may use
`child.parent(parent)` to define an authored hierarchy. SCN V1 remains readable;
opening it in the editor marks it for migration and the next save writes V2.
The complete visible composition is authored here: ground, walls and test
geometry are explicit `Primitive` objects rather than entities injected by the
engine. Primitive transform scale defines its dimensions. Clicking an imported
model section selects its authored SCN root.
The Editor File menu provides New/Open/Reload/Save/Save As, and the hierarchy
supports undoable creation, duplication, recursive deletion and reparenting.
Select an authored object and use `G`, `R` or `S` for viewport gizmos.

Scene scripts have no IO, OS, package-loading or debug libraries. CMake fetches
the pinned official Lua 5.5.0 runtime, Dear ImGui and ImGuizmo automatically;
no system packages are required for them.

If you need `compile_commands.json` at the repo root for tooling:

```bash
cmake -S . -B build -DCMAKE_EXPORT_COMPILE_COMMANDS=ON
ln -s build/compile_commands.json compile_commands.json
```

# Documentation

Generate the Doxygen docs (requires `doxygen` in your `PATH`):

```bash
doxygen Doxyfile
```

Open `docs/html/index.html` in a browser.

# Clean

```bash
rm -rf build
```

# Dependencies

This project requires the following libraries:

- **SDL2** (windowing, input, audio)
- **glad** (OpenGL loader — **Windows only** for bindings)
- **spdlog** (logging)
- **glm** (math)
- **Lua 5.5.0** (fetched and built automatically for restricted scene assets)
- **Dear ImGui / ImGuizmo** (fetched automatically for editor UI and gizmos)

Below are installation instructions for **macOS**, **Linux**, and **Windows (via vcpkg)**.

---

## macOS

Use **Homebrew**:

```bash
brew update
brew install sdl2 spdlog glm
```

Notes:
- `glad` is typically generated or bundled on macOS/Linux and is **not required** here.
- Ensure CMake can find Homebrew packages (usually automatic on Apple Silicon and Intel).

---

## Linux

### Debian / Ubuntu

```bash
sudo apt update
sudo apt install libsdl2-dev libspdlog-dev libglm-dev
```

Notes:
- As on macOS, `glad` is not required unless you explicitly choose to use it.

---

## Windows (vcpkg)

On Windows, dependencies are installed using **vcpkg**.

### 1. Install vcpkg (once)

```sh
git clone https://github.com/microsoft/vcpkg.git
cd vcpkg
./bootstrap-vcpkg.bat
```

### 2. Install required libraries

```sh
vcpkg install sdl2 spdlog glad glm
```

This project should work with every triplets type ( x86 / x64 ) ( dynamic / static ) however some triplets may require additional configuration.

### 3. Configure CMake with vcpkg toolchain

From the project root:

```powershell
cmake -S . -B build -DCMAKE_TOOLCHAIN_FILE=<path_to_vcpkg>/scripts/buildsystems/vcpkg.cmake
cmake --build build
```

Replace `<path_to_vcpkg>` with the actual path to your vcpkg directory.

Notes:
- `glad` is required on Windows to provide OpenGL function bindings.
- All dependencies are resolved automatically by vcpkg when using the toolchain file.
