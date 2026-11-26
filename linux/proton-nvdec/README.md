# Proton NVDEC Hardware Decoder Bridge (`linux/proton-nvdec`)

Drop-in **NVIDIA NVDEC GPU hardware decoder proxy** for **Veyra 2.2.x** running under **Proton / Wine** on Linux.

## Problem
Under Proton, `vkd3d-proton` does not yet implement `ID3D12VideoDevice` (D3D12VA), so Veyra's PS5 Remote Play falls back to single-threaded CPU software decoding (`thread_count = 1`). On `35–50 Mbps` 1080p H.265 scenes, single-thread CPU decode spikes to `30–85 ms`, causing queue drops and DLSS Frame Generation history resets.

## Solution
- **`avcodec-63.dll`** ([src/avcodec_nvdec_proxy.c](src/avcodec_nvdec_proxy.c)) forwards all 169 untouched FFmpeg 9.0.1 exports via PE export forwarding to `avcodec-63-orig.dll`.
- Intercepts Annex-B **H.265/HEVC** and **H.264** Remote Play decode contexts (`extradata_size == 0`, `hw_device_ctx == NULL`) and decodes directly on NVIDIA **NVDEC** via `nvcuda.dll` + `nvcuvid.dll` ([`SveSop/nvidia-libs`](https://github.com/SveSop/nvidia-libs)) with `ulMaxDisplayDelay = 0` (1-frame-in / 1-frame-out), outputting standard `NV12` / `P010LE` `AVFrame` buffers allocated via Veyra's own `avutil-61.dll`.
- When bypassed (`VEYRA_NVDEC=0`), upgrades software fallback from `thread_count = 1` to 4-thread `FF_THREAD_SLICE` (zero added frame buffering delay).

## Build
Requires `gcc-mingw-w64-x86-64-posix`, FFmpeg `n9.0.1` headers (`libavcodec`, `libavutil`), and `nv-codec-headers`.

```bash
./linux/proton-nvdec/scripts/build.sh
```

## Runtime environment variables
- `VEYRA_NVDEC=0` — bypass NVDEC and use 4-thread slice CPU software decode
- `VEYRA_NVDEC_LOG=1` — enable verbose logging (writes `/tmp/veyra-nvdec-proxy.log` inside the Wine environment)
- `VEYRA_ALWAYS_FOCUS=0` — disable the in-process `Qt6Gui!QGuiApplication::focusWindow()` IAT auto-focus guard (enabled by default to keep DualSense controller forwarding active across Wine/Xwayland focus drops)
