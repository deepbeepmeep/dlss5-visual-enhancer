# DLSS / ReShade runtime files (not in git — restore manually)

The proprietary, closed-source, or prebuilt Windows runtime files are
intentionally not included in this repository. Several exceed GitHub's
100 MB per-file push limit, so the full set ships only inside the portable
ZIP published under GitHub Releases. The repository's MIT license does not
cover these files and grants no right to obtain or redistribute them.

## Required layout

`src/core/runtime.py::validate_runtime_files()` enforces this exact
`host/` / `dlss/` / `dlssg/` split — flat layouts are rejected at startup:

```text
bin/runtime/host/dxgi.dll                <- ReShade 6.8.0 x64 WITH full add-on support
bin/runtime/host/LICENSE-ReShade.txt     <- BSD-3-Clause, committed in this folder
bin/runtime/host/renodx-dlss5.addon64    <- RenoDX nightly-era build v0.2026.0828.0517
bin/runtime/host/LICENSE-RenoDX.txt      <- MIT, committed in this folder
bin/runtime/host/nvngx_dlssnr.dll        <- NVIDIA DLSSNR 310.8.0.0 (DLSS 5 Neural Rendering)
bin/runtime/host/LICENSE-NVIDIA-DLSS.txt <- NVIDIA RTX SDKs License, committed in this folder
bin/runtime/host/nvngx.dll               <- project-built D3D12 worker (MIT, see below)
bin/runtime/host/nr-depth-worker.exe      <- WanGP depth-aware D3D12 worker (MIT source in native/WanGP-Adapter)
bin/runtime/dlss/nvngx_dlss.dll          <- NVIDIA DLSS 310.8.0.0 (Super Resolution)
bin/runtime/dlss/LICENSE-NVIDIA-DLSS.txt <- same NVIDIA text, keep a copy next to the DLL
bin/runtime/dlssg/nvngx_dlssg.dll        <- NVIDIA DLSS-G 310.7.0.0 (Frame Generation, NGX path)
bin/runtime/dlssg/dlssg-worker.exe       <- project-built DLSSG worker (MIT)
bin/runtime/dlssg/LICENSE-NVIDIA-DLSS.txt<- same NVIDIA text, keep a copy next to the DLLs
```

`ReShade.ini` is NOT listed: it is auto-generated runtime state. The
worker rewrites every `[RenoDX.DLSS5]` key from the UI settings on each
run — never commit it.

## Where to get each file

- **ReShade (`dxgi.dll`):** https://reshade.me/ — download **ReShade 6.8.0
  with full add-on support** (the standard build cannot load unsigned
  `.addon64` files). Rename the 64-bit DLL to `dxgi.dll`.
- **RenoDX add-on:** https://github.com/clshortfuse/renodx/releases —
  nightly-era build `v0.2026.0828.0517`, file `renodx-dlss5.addon64`.
  The RenoDX framework is MIT-licensed; this names the framework license,
  not a separate grant for the binary — use only builds you are
  authorized to redistribute.
- **NVIDIA DLLs (`nvngx_dlss.dll`, `nvngx_dlssg.dll`, `nvngx_dlssnr.dll`):**
  genuine NVIDIA SDK/driver/game distributions only
  (https://github.com/NVIDIA/DLSS, NVIDIA App, GeForce drivers). They are
  governed by the [NVIDIA RTX SDK License](https://github.com/NVIDIA/DLSS/blob/main/LICENSE.txt):
  no standalone redistribution, NVIDIA-GPU-only use, and no implying
  NVIDIA sponsorship. The DLSSNR runtime first shipped inside a game
  build, so verify authenticity (FileVersion `310.8.SF.0`,
  `NVIDIA DLSSNR`) before use.
- **Project-built (`host/nvngx.dll`, `dlssg/dlssg-worker.exe`):** built
  from the project's own MIT-licensed native sources
  (`DLSS5-Feeder`, `Frame-Interpolation`; not published in this repo).
  No download needed if you have the release ZIP; otherwise build them
  from the published sources.
- **WanGP adapter (`host/nr-depth-worker.exe`):** build it from
  `native/WanGP-Adapter`. It accepts WanGP's explicit color, motion, and
  depth stream and uses the same separately installed DLSS/ReShade/RenoDX
  runtime. It is included in WanGP worker-bundle release assets.

## Integrity checklist (SHA256 of the tested files)

```text
dxgi.dll                0CEE63F9C9F13F3AC909C5B4903F4DBB4B719A7AB3B4F13B0DEAF83C814B94F7
renodx-dlss5.addon64    D5ADF82EB44B065F4C590AC91FE824BAB07AFEA0EB9F994BDE936710C8593952
nvngx_dlssnr.dll        6EB209E764F39872625DEBD6ABAF45E2BB6322F6F270F781F70C059AE30B3927
nvngx_dlss.dll          C85F971CE023C9F3492FC7455F0B01A24BA18EA39636407A846902C4360B0B7E
nvngx_dlssg.dll         135EAF0733C1E37381A8C28ABCF7A862404A54132B81787C04E35D09EFC5E36F
```

This project is not affiliated with or endorsed by NVIDIA, ReShade, or RenoDX.
