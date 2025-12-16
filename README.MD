# Build

1. Configure the build directory: `cmake -S . -B build`
2. Build the project: `cmake --build build`
3. If you need `compile_commands.json` at the repo root for tooling, create a symlink to the generated file in `build`: `ln -s build/compile_commands.json compile_commands.json`

# Clean

Remove the build directory: `rm -rf build`
