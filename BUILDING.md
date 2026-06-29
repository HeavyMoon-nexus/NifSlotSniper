# Building NifSlotSniper_GUI

`NifSlotSniper_GUI.exe` is the C++ GUI application. It also uses a small companion CLI,
`slottool.exe`, which is built from a separate repository
(https://github.com/HeavyMoon-nexus/slottool — see its `BUILDING.md`).

## Requirements

- Windows 10/11
- Visual Studio 2022 with the **Desktop development with C++** workload
  (platform toolset **v143**, Windows 10 SDK, C++17)
- [vcpkg](https://github.com/microsoft/vcpkg)

## Dependencies

Supplied by **vcpkg**:

- nifly
- Dear ImGui (with the GLFW and OpenGL3 bindings)
- GLFW3
- GLAD (loader)
- GLM

Bundled **in this repository** (no action needed):

- nlohmann/json — `nlohmann/json.hpp`
- TinyXML-2 — `tinyxml2.cpp` / `tinyxml2.h`

## Steps

1. Clone and bootstrap vcpkg, then enable the MSBuild/Visual Studio integration:

   ```
   git clone https://github.com/microsoft/vcpkg
   .\vcpkg\bootstrap-vcpkg.bat
   .\vcpkg\vcpkg integrate install
   ```

2. Install the dependencies for x64:

   ```
   .\vcpkg\vcpkg install nifly "imgui[glfw-binding,opengl3-binding]" glfw3 "glad[loader]" glm --triplet x64-windows
   ```

   > A `vcpkg.json` manifest declaring the same dependencies is included in this repo for
   > reference and for vcpkg manifest-mode users.

3. Build the **x64 / Release** configuration. Either open `NifSlotSniper_GUI.vcxproj` in
   Visual Studio 2022 (set Configuration = Release, Platform = x64) and build, or from a
   *Developer Command Prompt for VS 2022*:

   ```
   msbuild NifSlotSniper_GUI.vcxproj /p:Configuration=Release /p:Platform=x64
   ```

   Build the **x64** configuration only (Win32 is not supported).

4. Output: `x64\Release\NifSlotSniper_GUI.exe`.

## Notes

- The project carries no explicit include/library paths; it relies on vcpkg's MSBuild
  integration (step 1) to provide them.
- A post-build message about `pwsh.exe` not being recognized is harmless (an optional
  post-build copy step); the compile and link still succeed.

## License

`NifSlotSniper_GUI` is MIT-licensed (see `LIcenses/LICENSE` and `THIRD_PARTY_NOTICES.md`).
The companion `slottool` CLI is a separate project licensed under GPL-3.0.
