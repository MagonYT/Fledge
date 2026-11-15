<div align="center">

# Fledge

### Build worlds. Bring them to life.

A C++20 game engine and desktop editor for building interactive worlds—
from scene authoring and gameplay scripting to a standalone game runtime.

**STANDARD EDITION · WINDOWS · C++20 · CMAKE**

[Get started](#getting-started) · [Explore the engine](#inside-fledge) · [Build & package](#build--package) · [Source guide](#source-guide) · [License](#license)

</div>

---

## A world is more than its geometry

Lighting, movement, sound, weather, and gameplay all contribute to how a world feels. Fledge brings these systems into one native editing workflow, with a separate runtime for the games you build.

The ambition is an approachable engine for expressive, interactive environments: author a scene, add behavior, experiment in the editor, and package a playable result. This repository publishes the **Standard edition source**, not the Advanced edition or a prebuilt binary release.

> **Development status**
>
> Fledge is under active development. The systems below are represented in the source; they are not a claim of production readiness or equal support across every graphics backend. This lean public export has not yet completed a full configure-and-build verification: dependency downloads interrupted validation. Treat it as development source, not a verified release build.

## Inside Fledge

### Author your world

- **Scene editing** — component-based scenes, hierarchy tools, asset workflows, and project templates.
- **Integrated authoring** — editor tools for animation, physics, audio, gameplay, graphics, and world systems.
- **C++ gameplay scripting** — Fledge Script SDK and hot-reload infrastructure for developing behavior alongside your project.
- **Project starting points** — Empty, Third Person, and City Sandbox templates included in the source tree.

### Shape the atmosphere

- **Physically based rendering** — PBR materials, image-based lighting, shadows, bloom, and tonemapping.
- **Screen-space effects** — ambient occlusion, reflections, depth of field, and motion blur.
- **Lighting and exposure** — irradiance probes, fog/light-shaft effects, and temporally adapted exposure.
- **Environment systems** — day/night and weather gameplay systems, with rain-driven wet-surface rendering.
- **Graphics backends** — source implementations for Direct3D 9/11/12, OpenGL, Vulkan, and a null backend. Feature coverage and hardware requirements vary.

### Make it move

- **Physics** — Jolt-based rigid bodies, character interaction, vehicles, joints, and ragdoll support.
- **Animation** — skeletal animation, blending, state machines, and inverse-kinematics systems.
- **Audio** — spatial sources, buses, streaming, reverb zones, and occlusion support.
- **Gameplay foundations** — player and driving systems, traffic, police, missions, navigation, and game UI.
- **Larger environments** — world partitioning/streaming, terrain, foliage, culling, and LOD infrastructure.
- **Networking** — client/server replication, prediction, and interpolation foundations.

### Connect your workflow

- **Asset import** — FBX, glTF, image/mesh workflows, and package-import tooling.
- **Blueprint conversion** — tools for translating supported Unreal Blueprint T3D text into Fledge Script C++, with conversion reporting. This is not a promise of lossless project conversion.
- **Optional AI assistance** — editor-side provider integrations for assisted authoring.
- **MCP tooling** — editor automation and a standalone MCP server.
- **Browser extensions** — source for Chromium and Firefox import integrations.

> **About weather and fluids**
>
> Weather-driven surface effects are not the same as a full fluid solver. Physically simulated rainfall, individual droplets on glass, and water accumulation with drainage are future ambitions—not features promised by this source release.

## Getting started

### 1. Prepare a Windows development environment

You will need:

- **CMake 3.24 or newer**.
- A **C++20-capable compiler and standard library**, including `std::format` support.
- **Ninja** when using a Ninja preset.
- **Git** and internet access for dependencies downloaded during configuration.
- Suitable graphics hardware and drivers for the backend you intend to use.

The supplied presets cover MinGW GCC, Clang, and Visual Studio 2022. A preset describes a configuration; its presence does not certify that toolchain against this export.

### 2. Get the source

```powershell
git clone https://github.com/MagonYT/Fledge.git
cd Fledge
```

### 3. Configure and build

With GCC and Ninja available on `PATH`:

```powershell
cmake --preset ninja-gcc-release
cmake --build --preset ninja-gcc-release
```

The editor executable is `Fledge.exe`, under the build directory's `bin` folder. For the Ninja release preset:

```powershell
.\build\ninja-gcc-release\bin\Fledge.exe
```

Multi-configuration generators may add a configuration subdirectory such as `Release`.

<details>
<summary><strong>Visual Studio 2022</strong></summary>

Use a Visual Studio developer terminal with the C++ desktop toolchain installed:

```powershell
cmake --preset vs2022
cmake --build build/vs2022 --config Release
```

This is an alternative configuration path, not a separately verified binary release.

</details>

<details>
<summary><strong>Debug builds and available presets</strong></summary>

List the available configure presets:

```powershell
cmake --list-presets
```

Build the GCC debug configuration:

```powershell
cmake --preset ninja-gcc
cmake --build --preset ninja-gcc
```

</details>

## Build & package

### Create a relocatable installation

For the Ninja release build:

```powershell
cmake --install build/ninja-gcc-release --prefix out/Fledge
```

For Visual Studio:

```powershell
cmake --install build/vs2022 --config Release --prefix out/Fledge
```

The install rules collect the runtime/editor tools, project templates, scripting SDK, resources, and license notices. Installing the complete layout is preferable to distributing a single executable without its supporting files.

### Create a portable archive

After a successful build:

```powershell
cmake --build build/ninja-gcc-release --target package
```

CPack produces the archive under the selected build directory's `dist` folder.

### Create an installer

The CMake packaging flow includes an NSIS installer target:

```powershell
cmake --build build/ninja-gcc-release --target installer
```

Installer tooling may require additional downloads. Packaging commands are provided by the build configuration, but have not been validated end-to-end for this export.

### Build options

- `FLEDGE_BUILD_EDITOR=OFF` excludes the desktop editor executable; it does not necessarily eliminate every authoring-tool dependency.
- `FLEDGE_DXR=OFF` disables build-time DXR shader compilation. It does **not** guarantee that all shader-compiler or upscaler downloads are skipped.
- The published source is **Standard-only**. Advanced configuration is explicitly rejected.
- The internal test suite is not included; test building is disabled in this distribution.

Example configuration without build-time DXR shader compilation:

```powershell
cmake --preset ninja-gcc-release -DFLEDGE_DXR=OFF
```

## Source guide

**Engine and gameplay** — [`engine/`](engine/)

Core utilities, platform abstraction, rendering, scene data, physics, animation, audio, assets, gameplay, networking, world systems, and the scripting SDK.

**Desktop authoring** — [`editor/`](editor/)

The editor application, UI, authoring services, project workflows, and automation integration.

**Game execution** — [`runtime/`](runtime/)

The standalone game runtime. Build-time dependency checks guard against linking editor, AI, and MCP libraries into the shipping runtime.

**Supporting tools** — [`tools/`](tools/)

The game packager, MCP server, and browser import extensions.

**Project templates** — [`templates/`](templates/)

Starter project content used by the editor's project-creation workflow.

**Build and distribution** — [`cmake/`](cmake/) · [`installer/`](installer/)

Dependency setup, resources, build configuration, install rules, and packaging sources.

**Resources and notices** — [`resources/`](resources/) · [`licenses/`](licenses/)

Required application artwork and third-party license material.

## What this repository includes

This is intentionally a focused source distribution:

- Standard engine, editor, and runtime source.
- Build configuration and packaging tools.
- Starter templates and required application resources.
- Project license and third-party notices.

It intentionally excludes Advanced implementation modules, the separate launcher, internal tests, sample-project collections, private development notes, personal editor settings, credentials, and generated build artifacts.

Some shared edition identifiers remain so unsupported project requirements can be recognized and rejected safely. Their presence does not enable Advanced functionality. Some in-editor help references may point to documentation that is not part of this lean distribution.

## Configuration & safety

### Keep credentials local

Configure optional provider credentials through the supported local credential/environment mechanisms. Never place real tokens in source files, templates, screenshots, or issue reports. The repository's ignore rules exclude common credential and local-settings files, but ignore rules are not a secret scanner.

AI/provider integrations are optional and subject to their providers' availability, API compatibility, terms, and costs. Some integration presets require further verification; do not assume that every listed provider is production-tested.

### Treat automation as privileged access

MCP tools can operate on editor projects. Keep automation endpoints local and restricted to trusted clients. When using the HTTP transport, configure `FLEDGE_MCP_TOKEN` as appropriate; authentication is optional in the implementation, so an unconfigured local endpoint should not be treated as protected from other local processes.

### Respect asset licenses

Import tooling does not grant rights to third-party content. You are responsible for ensuring imported assets, fonts, audio, and other material can be used and redistributed in your project.

## Troubleshooting

<details>
<summary><strong>Configuration appears to pause during downloads</strong></summary>

The first configure can fetch substantial third-party dependencies. Check the most recent CMake output, network/proxy configuration, and access to upstream download hosts before assuming a compiler failure. DXC and XeSS-related downloads interrupted validation of this export.

Do not disable checksum verification or substitute untrusted archives to get past a failed download.

</details>

<details>
<summary><strong>CMake reports FetchContent deprecation warnings</strong></summary>

Newer CMake versions may warn about dependency-population APIs used by the build scripts. A warning alone is not proof of a failed build. Locate the first actual error and include the relevant output when reporting a problem.

</details>

<details>
<summary><strong>A project requires an unsupported edition or feature</strong></summary>

This distribution builds only Fledge Standard. Projects whose manifests or scene content require unsupported Advanced features are rejected rather than silently packaged with a missing runtime. Use content supported by Standard; changing a project label alone does not convert its features.

</details>

## Reporting problems

Open an [issue](https://github.com/MagonYT/Fledge/issues) with:

- A concise description and reproducible steps.
- The source revision and selected CMake preset.
- Compiler, CMake, Windows, GPU, and driver versions where relevant.
- Expected behavior, actual behavior, and the first meaningful error.
- A minimal reproduction that you have permission to share.

Remove tokens, personal paths, proprietary assets, and private project data before uploading logs or reproductions. Do not publish credentials or exploit details in a public issue.

## License

**No open-source license has been selected.** The current [LICENSE](LICENSE) reserves all rights to the repository owner. Public source visibility does not by itself grant permission to reuse, modify, or redistribute the project.

Third-party dependencies retain their own licenses. See [THIRD_PARTY_LICENSES.md](THIRD_PARTY_LICENSES.md) and the [`licenses/`](licenses/) directory. Those notices must be considered separately from the project's own licensing terms.

---

<div align="center">

**Fledge** · A native toolset for building worlds with character.

</div>
