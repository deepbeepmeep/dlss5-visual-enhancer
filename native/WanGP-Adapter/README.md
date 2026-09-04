# WanGP native adapters

This directory contains the source for WanGP's Neural Rendering and Frame Generation workers. The Neural Rendering worker is derived from `jlrouzies-fr/DLSS5-Feeder` commit `792755324574ff4703eb441a7bc14c724e125b84` under the included MIT license.

The `--wangp-video` mode is a bounded-memory binary stream. WanGP sends one frame at a time with:

- RGBA8 color;
- R16G16_FLOAT current-to-previous motion vectors in pixel units;
- R32_FLOAT reversed-Z depth;
- a reset flag and presentation timestamp.

The worker invokes the public DLSS/DLAA feature-1 API. A separately installed RenoDX DLSS 5 add-on intercepts that call and performs feature 18 Neural Rendering. No private feature-18 ABI is copied into this source.

`dlssg_worker.cpp` is a direct D3D12 host for the documented NGX DLSS Frame Generation API. It preserves the original Merserk worker's bounded streaming protocol, but records every Multi Frame Generation index for a rendered frame in one GPU command list. This makes native 2x through 6x return distinct intermediate frames when the hardware and NGX runtime expose the requested count. It also publishes a stable `DLSSG.BackbufferFrameID` for each group. This is an independent implementation; no Merserk machine code or unpublished source is included.

## Build

Install Visual Studio 2022 C++ build tools and a Windows SDK, clone NVIDIA's official DLSS SDK, then run from the repository root:

```powershell
powershell -ExecutionPolicy Bypass -File native\WanGP-Adapter\build.ps1 -NgxSdk C:\temp\NVIDIA-DLSS
```

The outputs are `bin/runtime/host/nr-depth-worker.exe` and `bin/runtime/dlssg/dlssg-worker.exe`. Pass `-Target nr` or `-Target dlssg` to build only one worker. NVIDIA SDK headers and import libraries are build inputs and are not included here.

## Runtime layout

The worker runs from `host/`, while its public DLSS feature runtime is discovered in sibling `dlss/` through `NVSDK_NGX_FeatureCommonInfo.PathListInfo`. Frame Generation runs independently from sibling `dlssg/`; this prevents its process from loading ReShade's `host/dxgi.dll`.

See the main README and WanGP installation guide for license, security, and third-party download requirements.
