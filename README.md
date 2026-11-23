# LumaCam for OBS

OBS Studio source plugin that receives the LumaCam iPhone camera stream (up to 4K60, HEVC / H.264)
over USB (usbmuxd) or Wi-Fi (Bonjour / manual IP) and decodes it in hardware.

Built on the official [obs-plugintemplate](https://github.com/obsproject/obs-plugintemplate).
See the [root README](../README.md) for build instructions and [docs/PROTOCOL.md](../docs/PROTOCOL.md) for the protocol.

| Path | Role |
|---|---|
| `src/plugin-main.cpp` | Module entry points |
| `src/lumacam-source.*` | The OBS source: worker thread, reconnection, properties, camera controls |
| `src/lcp/protocol.hpp` | LCP framing and Annex B helpers |
| `src/net/socket.*` | Cross-platform TCP / Unix socket with timeouts |
| `src/net/usbmux.*` | usbmuxd client (iPhone over USB) |
| `src/net/discovery.*` | Bonjour browser (`_lumacam._tcp`) |
| `src/decoder/vt-decoder.cpp` | VideoToolbox hardware decoder (macOS) |
| `src/decoder/ffmpeg-decoder.cpp` | FFmpeg decoder with D3D11VA / DXVA2 / VAAPI / CUDA (Windows, Linux) |
| `data/locale/` | English and French strings |

License: GPL-2.0-or-later.
