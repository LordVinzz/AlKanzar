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
`V1SCN-----`; the remaining lines use the restricted scene DSL:

```lua
scene = Create({ type = "Scene" })
object = Create({ type = "Model", name = "Object", asset = "Object.glb" })
object.transform({ position = { x = 0, y = 0, z = 0 } })
scene.add(object)
scene.build()
```

Scene scripts have no IO, OS, package-loading or debug libraries. CMake fetches
the pinned official Lua 5.5.0 runtime automatically; no system Lua package is
required.

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
