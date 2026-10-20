# WanGP native adapters

This directory contains the source for WanGP's Neural Rendering and Frame Generation workers. The Neural Rendering worker is derived from `jlrouzies-fr/DLSS5-Feeder` commit `792755324574ff4703eb441a7bc14c724e125b84` under the included MIT license.

The `--wangp-video` mode is a bounded-memory binary stream. WanGP sends one frame at a time with:

- RGBA8 color;
- R16G16_FLOAT current-to-previous motion vectors in pixel units;
- R32_FLOAT reversed-Z depth;
- a reset flag and presentation timestamp.

The worker invokes the public DLSS/DLAA feature-1 API. A separately installed RenoDX DLSS 5 add-on intercepts that call and performs feature 18 Neural Rendering. No private feature-18 ABI is copied into this source.

WanGP passes `--nr-intensity 0..2` with `--wangp-video`. The worker validates it and writes RenoDX's native `NRIntensity` setting before ReShade loads; `1` is the native default.

Spatial output is limited to an orientation-independent 8K boundary: at most 7680 pixels on the long edge and 4320 pixels on the short edge.

`dlssg_worker.cpp` is a direct D3D12 host for the documented NGX DLSS Frame Generation API. It preserves the original Merserk worker's bounded streaming protocol, but records every Multi Frame Generation index for a rendered frame in one GPU command list. This makes native 2x through 6x return distinct intermediate frames when the hardware and NGX runtime expose the requested count. It also publishes a stable `DLSSG.BackbufferFrameID` for each group. This is an independent implementation; no Merserk machine code or unpublished source is included.

## Build on Windows

Install Visual Studio 2022 C++ build tools and a Windows SDK, clone NVIDIA's official DLSS SDK, then run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File native\WanGP-Adapter\build.ps1 -NgxSdk C:\temp\NVIDIA-DLSS
```

The outputs are `bin/runtime/host/nr-depth-worker.exe` and `bin/runtime/dlssg/dlssg-worker.exe`. Pass `-Target nr` or `-Target dlssg` to build only one worker. NVIDIA SDK headers and import libraries are build inputs and are not included here.

## Linux download (experimental Frame Generation x2)

The [Linux v0.1.0 prerelease](https://github.com/DeepBeepMeep/dlss5-visual-enhancer/releases/tag/wangp-linux-v0.1.0) provides a compiled x86_64 worker. Extract `WanGP-DLSSG-linux-x86_64-v0.1.0.tar.gz` into `WanGP/dlss5`, then obtain `libnvidia-ngx-dlssg.so.310.9.16` separately from the pinned NVIDIA SDK below and place it beside `dlssg-worker`. The archive includes license notices and checksums, but no NVIDIA runtime shared library.

The compiled worker requires glibc 2.34 or newer, `libstdc++.so.6` providing `GLIBCXX_3.4.32` or newer, and a Vulkan loader (`libvulkan.so.1`), in addition to a compatible NVIDIA Vulkan driver and GPU. Ubuntu 24.04 is an example of a compatible userspace.

**WSL Neural Rendering does not need this Linux worker or the Linux NVIDIA runtime.** Use WanGP's Windows installer and existing Windows Neural Rendering worker for that workflow. This package is for the separate experimental native Linux Frame Generation x2 path.

## Build on Linux (experimental Frame Generation x2)

`dlssg_worker_vk.cpp` hosts the public NGX Vulkan API on Linux x86_64. It uses the same `--probe` and `--serve` interface as the Windows Frame Generation worker, with one generated frame per input interval (x2). It was ported from WanGP's experimental Linux worker. Native Linux Neural Rendering and multipliers above x2 are not implemented. The standalone Gradio application remains Windows-only; this worker is intended for WanGP's Linux integration.

Install a C++17 compiler, Vulkan development headers (1.3.239 or newer, including `VK_NV_optical_flow`), a Vulkan loader, and Git. For example, on Ubuntu 24.04:

```bash
sudo apt install g++ libvulkan-dev git
git clone --filter=blob:none --no-checkout https://github.com/NVIDIA/DLSS.git NVIDIA-DLSS
git -C NVIDIA-DLSS sparse-checkout set include lib/Linux_x86_64
git -C NVIDIA-DLSS checkout 9981c6350dc58c0ce21f024fd65e31e851c31689
bash native/WanGP-Adapter/build_linux.sh "$PWD/NVIDIA-DLSS"
./bin/runtime/dlssg/dlssg-worker --probe
```

The build uses the pinned DLSS 310.9.16 SDK and installs `dlssg-worker`, `libnvidia-ngx-dlssg.so.310.9.16`, `LICENSE-NVIDIA-DLSS-Linux.txt`, and `LICENSE-WanGP-Adapters.txt` in `bin/runtime/dlssg`. Review NVIDIA's `LICENSE.txt`; the SDK/runtime are separate build inputs and are not vendored here. Set `CXX` to choose another compiler.

To install directly into a WanGP checkout, pass its runtime directory as the second argument:

```bash
bash native/WanGP-Adapter/build_linux.sh "$PWD/NVIDIA-DLSS" /path/to/WanGP/dlss5/dlssg
```

Restart WanGP and select **DLSS Frame Generation x2**. The worker discovers the NVIDIA runtime beside its executable, independently of the current working directory. It requires an NVIDIA GPU exposed through Vulkan and a driver supporting NGX Frame Generation. WSL CUDA support or successful WSL Neural Rendering through a Windows worker does not establish that requirement.

**NVIDIA frame generation and output quality remain unvalidated on native Linux.** Compilation, protocol failure checks, and the Vulkan transfer diagnostic do not exercise DLSS inference. Before treating this as production-ready, test motion, scene cuts, repeated sessions, and cancellation on supported NVIDIA hardware. Input motion is estimated by WanGP, and the worker uses constant depth, so occlusions and fine details may exhibit artifacts.

## Linux diagnostics and protocol

`--probe` writes one JSON object to stdout, including `available`, `worker_version`, and `multi_frame_count_max` (1). Failures return nonzero and include `reason`; diagnostics go to stderr. `--self-test` checks Vulkan upload/readback without calling DLSS and may run on a software Vulkan device:

```bash
./bin/runtime/dlssg/dlssg-worker --self-test
python3 native/WanGP-Adapter/test_linux_worker.py ./bin/runtime/dlssg/dlssg-worker
python3 native/WanGP-Adapter/test_linux_worker.py ./bin/runtime/dlssg/dlssg-worker --transfers
```

The optional transfer check needs an available Vulkan device; CI uses Mesa software rendering. Enable `VK_INSTANCE_LAYERS=VK_LAYER_KHRONOS_validation` to check transfers with the Vulkan validation layer installed.

`--serve` uses little-endian binary stdin/stdout. All replies are four `uint32` values:

| Message | Fields |
| --- | --- |
| Setup (`<5I`) | Magic `0x31534746`, width, height, input frame count, generated count (must be 1) |
| Setup reply (`<4I`) | Magic `0x31524746`, status (0 succeeds), maximum generated count, reserved |
| Frame (`<4I2q`) | Magic `0x31464746`, sequential zero-based index, reset (0/1), reserved (0), timestamp numerator, positive denominator |
| Frame payload | Packed RGBA8 color, then packed RG16F current-to-previous motion in pixel units |
| Frame reply (`<4I`) | Magic `0x314f4746`, status, generated count, interpolation disabled |
| Output payload | One packed RGBA8 frame only when generated count is 1 |

The first input or a reset primes history and returns no generated frame. NGX may also disable interpolation. Dimensions must be positive, no axis may exceed 8192 pixels, and the total must not exceed 7680 × 4320 pixels. The worker exits after the declared frame count; truncated streams and invalid headers fail explicitly. Timestamps are checked for validity but do not drive NGX pacing: each generated image represents the midpoint between successive inputs.

## Runtime layout

The worker runs from `host/`, while its public DLSS feature runtime is discovered in sibling `dlss/` through `NVSDK_NGX_FeatureCommonInfo.PathListInfo`. Frame Generation runs independently from sibling `dlssg/`; this prevents its process from loading ReShade's `host/dxgi.dll`.

See the main README and WanGP installation guide for license, security, and third-party download requirements.
