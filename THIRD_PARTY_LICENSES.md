# Third-party components

Fledge builds on the following third-party components. Every one is pinned:
- FetchContent dependencies are pinned to a git tag or commit, or to a SHA256-pinned archive.
- Downloaded fonts are SHA256-verified.

The full license texts are in [`licenses/`](licenses/). Installs and the portable ZIP ship them in `<install>/licenses/`.

| Component | Version | License | Used by | License file |
|---|---|---|---|---|
| GLFW | 3.4 | zlib/libpng | engine (platform), editor, runtime | `licenses/GLFW-zlib.txt` |
| Dear ImGui (docking branch) | v1.91.9b-docking | MIT | editor only | `licenses/DearImGui-MIT.txt` |
| nlohmann/json | 3.11.3 | MIT | engine | `licenses/nlohmann_json-MIT.txt` |
| stb (stb_image, stb_image_write, stb_dxt, stb_easy_font, stb_truetype) | commit d14d46f6 | MIT or Public Domain | engine, tools, runtime settings menu | `licenses/stb-MIT-or-PublicDomain.txt` |
| microsoft/DirectX-Headers (headers only) | v1.620.1 (SHA-256 pinned archive) | MIT | engine (Direct3D 12 backend, DXR) | `licenses/DirectX-Headers-MIT.txt` |
| KhronosGroup/Vulkan-Headers (headers only) | v1.4.367 (SHA-256 pinned archive) | Apache-2.0 OR MIT | engine (Vulkan probe) | `licenses/Vulkan-Headers-Apache-2.0-MIT.txt` |
| AMD FidelityFX Super Resolution 1 (`ffx_a.h`, `ffx_fsr1.h`, embedded unmodified as shader text) | v1.0.2 (SHA-256 pinned archive) | MIT | engine (FSR 1 passes) | `licenses/FidelityFX-FSR1-MIT.txt` |
| meshoptimizer | 0.22 (SHA-256 pinned archive) | MIT | engine (mesh LOD chains, HLOD proxies; runtime + cooker) | `licenses/meshoptimizer-MIT.txt` |
| Jolt Physics | v5.2.0 (SHA-256 pinned archive, `CROSS_PLATFORM_DETERMINISTIC` build) | MIT | engine physics (rigid bodies, characters, vehicles, ragdolls; runtime + editor) | `licenses/JoltPhysics-MIT.txt` |
| miniaudio | 0.11.21 (SHA-256 pinned archive) | Public Domain (Unlicense) or MIT No Attribution | engine audio (device output, WAV / MP3 / FLAC decoding; runtime + editor) | `licenses/miniaudio-Unlicense-or-MIT0.txt` |
| stb_vorbis | v1.22, from miniaudio 0.11.21 `extras/` (same pinned archive) | MIT or Public Domain | engine audio (Ogg Vorbis decoding; runtime + editor) | `licenses/stb-MIT-or-PublicDomain.txt` |
| Recast & Detour (recastnavigation) | v1.6.0 (SHA-256 pinned archive) | zlib | engine navigation meshes + path queries (runtime + editor) | `licenses/RecastNavigation-zlib.txt` |
| cgltf | v1.15 | MIT | editor / tools importers only | `licenses/cgltf-MIT.txt` |
| ufbx (`ufbx.c`, `ufbx.h`) | v0.23.1 (SHA-256 pinned archive `21edd102…f19c956`) | MIT or Public Domain (Unlicense), user's choice; Fledge uses it under MIT | editor / tools importers only (FBX import, 1.1.0); never in the runtime or packaged games | `licenses/ufbx-MIT-or-Unlicense.txt` |
| doctest | 2.4.11 | MIT | unit tests only (not shipped) | `licenses/doctest-MIT.txt` |
| Roboto Medium (font, from Dear ImGui `misc/fonts`) | as shipped with ImGui v1.91.9b | Apache-2.0 | editor UI font | `licenses/Roboto-Cousine-Apache-2.0.txt` |
| Cousine Regular (font, from Dear ImGui `misc/fonts`) | as shipped with ImGui v1.91.9b | Apache-2.0 | editor monospace font (log, code) | `licenses/Roboto-Cousine-Apache-2.0.txt` |
| Font Awesome Free, `fa-solid-900.ttf` | 6.5.2 | Font: SIL OFL 1.1; icons: CC BY 4.0; code: MIT | editor icon font | `licenses/FontAwesomeFree-OFL-1.1-CC-BY-4.0-MIT.txt` |
| JetBrains Mono Regular (font) | v2.304 (SHA-256 pinned download) | SIL OFL 1.1 | editor Code workspace font | `licenses/JetBrainsMono-OFL-1.1.txt` |
| clangd (`clangd.exe` + clang builtin headers, from `clangd-windows-23.1.0.zip`) | 23.1.0 (SHA-256 pinned: 23412a24…8c810) | Apache-2.0 WITH LLVM-exception | editor Code workspace language server (separate process, shipped in `resources/clangd/`) | `licenses/clangd-LLVM-Apache-2.0-with-LLVM-exception.txt` |
| NSIS (build tool only, never shipped) | 3.10 | zlib/libpng and others | `installer` target only; it compiles `installer/fledge.nsi` | not redistributed (the NSIS stub inside the setup exe is zlib-licensed) |
| DirectX Shader Compiler (`dxcompiler.dll`) | v1.9.2609, `dxc_2026_09_29.zip` (SHA-256 pinned) | LLVM Release License (University of Illinois/NCSA) + MIT. The Microsoft licence in the archive covers `dxil.dll`, which Fledge does not ship | build time: compiles `engine/render/shaders/raytracing.hlsl` to embedded DXIL. Runtime: `dxcompiler.dll` ships next to the editor, the tools and every packaged game, and the Vulkan backend compiles HLSL to SPIR-V with it | `licenses/DirectXShaderCompiler-NCSA-MIT.txt` |
| Intel XeSS SDK headers `xess.h`, `xess_d3d12.h` (build input only, never shipped) | v3.0.2 (SHA-256 pinned at configure time) | Intel Simplified Software License | compile the D3D12 XeSS bridge | not redistributed |

Font Awesome attribution, as required by CC BY 4.0: icons by Fonticons, Inc. (https://fontawesome.com), licensed under CC BY 4.0.

Packaged games (`fledgepack` output) contain:
- the Fledge runtime, which uses GLFW, nlohmann/json, stb, meshoptimizer, Jolt Physics, miniaudio, Recast & Detour, DirectX-Headers, Vulkan-Headers and FidelityFX FSR 1, and loads the DirectX Shader Compiler (`dxcompiler.dll`) for Vulkan
  (fledgepack copies their licence texts into `<game>/licenses/`);
- your content;
- optionally the vendor upscaler runtime the project selected (below), with its licence.

They do not contain ImGui, cgltf, ufbx, fonts, AI or MCP code.

## Store assets you import

Assets imported from the Unity Asset Store, Fab or elsewhere keep their publishers' licences; Fledge does not relicense
them. The editor records where each one came from and exports a "Third-party assets" list (Asset Store Import panel,
or the palette command *Export Third-party Assets List*). See `docs/ASSET_STORES.md`.

## Vendor upscaler runtimes (downloaded on demand, never in this repository)

The editor's upscaler versions manager (see `docs/GRAPHICS.md`) can download these **only after the user has read
and accepted the vendor licence in the editor**; every file is verified against a SHA-256 pinned in
`engine/render/src/upscaler_catalog.cpp`. Fledge itself contains none of them.

| Component | Versions | Licence (shown before download, copied next to packaged games) |
|---|---|---|
| NVIDIA DLSS Super Resolution `nvngx_dlss.dll` (github.com/NVIDIA/DLSS) | 3.1.0 - 310.9.16 | NVIDIA RTX SDKs License (`LICENSE.txt` of the same tag) |
| Intel XeSS `libxess.dll` (github.com/intel/xess) | 2.0.1 - 3.0.2 | Intel Simplified Software License (`LICENSE.txt` of the same tag) |

## C++ toolchain for Fledge Script files (downloaded on demand, never in this repository or the installer)

Editor Settings > Scripts > *Install C++ toolchain* can download a portable MinGW-w64 compiler **only after the user
has read the licence summary and pressed Accept**. The archive is checked against a size and SHA-256 pinned in
`editor/core/src/toolchain_installer.cpp`. It is unpacked into `<user data>/Toolchains/` and is not redistributed by
Fledge or included in packaged games (games only contain the compiled script DLL).

| Component | Version | Licences |
|---|---|---|
| WinLibs MinGW-w64 GCC (github.com/brechtsanders/winlibs_mingw), `winlibs-x86_64-posix-seh-gcc-16.2.0-mingw-w64msvcrt-14.0.0-r3.zip`, SHA-256 `62af32ea58acdea1318ea5dd8eb7017eaee694dd40b269d52a59c769dcfefb82` | GCC 16.2.0, MinGW-w64 14.0.0 | GCC: GPL-3.0-or-later; libgcc / libstdc++: GPL-3.0 with the GCC Runtime Library Exception (built programs are not GPL-bound); binutils: GPL-3.0-or-later; MinGW-w64 runtime / headers: ZPL-2.1 and MIT-style; winpthreads: MIT / BSD-style |
