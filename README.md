<div align="center">

# termuxwc

**A lightweight, high-performance tiling Wayland compositor built from scratch with wlroots for rootless Android (Termux).**

[![License: MIT](https://img.shields.io/badge/License-MIT-blue.svg)](LICENSE)
[![Platform](https://img.shields.io/badge/Platform-Android%20%7C%20Linux%20(aarch64)-green.svg)](https://termux.dev)
[![wlroots](https://img.shields.io/badge/wlroots-0.19.1-orange.svg)](https://gitlab.freedesktop.org/wlroots/wlroots)
[![Wayland](https://img.shields.io/badge/Protocol-Wayland-red.svg)](https://wayland.freedesktop.org)

[Quickstart](#quick-start) •
[Features](#features) •
[Keybindings](#keybindings) •
[Configuration](#configuration) •
[GPU Acceleration](#hardware-gpu-acceleration) •
[Build from Source](#building-from-source)

</div>

---

## Overview

`termuxwc` brings a complete, SwayWM-grade tiling desktop experience to mobile devices. It runs natively within Termux userspace without requiring root privileges, custom kernels, or `proot` containers. 

Designed specifically for touchscreens and mobile workflows, `termuxwc` pairs dynamic master-stack tiling with low-latency remote desktop streaming via `wayvnc`, allowing you to use your Android phone or tablet as a complete Linux productivity workstation.

---

## Quick Start

### Automatic Installation (Recommended)

Run the one-line installer inside Termux:

```bash
curl -sL https://raw.githubusercontent.com/Oflucoder/termuxwc/main/install.sh | bash
```

The installer automatically:
1. Installs all required Termux dependencies (`wayvnc`, `foot`, `virglrenderer-android`, `pixman`, `libxkbcommon`).
2. Deploys the precompiled, Android-patched `libwlroots-0.19.so` and `termuxwc` binaries.
3. Generates the default configuration file (`~/.config/termuxwc/config`).
4. Adds the global `termuxwc-session` command to your path.

### Launching the Desktop

Start the desktop session:

```bash
termuxwc-session
```

Then connect any VNC viewer:
* **On your phone**: [AVNC](https://github.com/gstansev/AVNC) (recommended) or [bVNC](https://github.com/bk138/multivnc) to `127.0.0.1:5900`
* **From a PC on the same Wi-Fi network**: `<phone-ip>:5900`

To shut down the session cleanly, press `Ctrl + C` in the Termux terminal.

---

## Features

- **Native & Rootless**: Runs directly inside Termux with no chroot or proot overhead.
- **Dynamic Mobile Tiling**:
  - Master-stack layout that automatically adapts to device orientation.
  - **Landscape**: Master window on the left, vertical stack on the right.
  - **Portrait**: Master window on top, horizontal stack below for single-hand mobile use.
- **9 Virtual Workspaces**:
  - Independent workspaces (`Mod + 1..9`) with zero background rendering overhead for inactive workspaces.
  - Instant window migration (`Mod + Shift + 1..9`).
- **Universal Mobile GPU Acceleration**:
  - Auto-detects physical GPU hardware: Qualcomm Adreno (`/dev/kgsl-3d0`), ARM Mali (`/dev/mali0`), PowerVR, Samsung Xclipse, and direct Linux DRM nodes.
  - Routes 3D OpenGL and GLES calls to the physical host GPU via a built-in VirGL bridge (`GALLIUM_DRIVER=virpipe`).
- **Customizable Theming & Aesthetics**:
  - Active and inactive window border outlines rendered via `wlr_scene_rect`.
  - Configurable inner and outer gaps.
- **Mobile-Friendly Mod Key (`ModBoth`)**:
  - Allows **Alt** and **Super (Windows)** keys to work interchangeably, enabling full keybinding support on mobile virtual keyboards (e.g. Hacker's Keyboard, Gboard).
- **Modern Wayland Protocol Support**:
  - **XDG-Shell** (`xdg_wm_base`): Window management, moving, resizing, fullscreen, and floating states.
  - **Layer-Shell** (`zwlr_layer_shell_v1`): Wallpapers (`swaybg`, `wpaperd`) and status bars (`waybar`).
  - **Virtual Input**: Full input pass-through for remote VNC keyboard and touch pointers.
  - **Screencopy**: Low-latency desktop capture via `zwlr_screencopy_manager_v1`.

---

## Keybindings

Default modifier: **`Mod`** (configured to accept either `Super` or `Alt`).

| Shortcut | Action | Description |
| :--- | :--- | :--- |
| **`Mod + Return`** | Launch Terminal | Opens `foot` terminal with GPU acceleration |
| **`Mod + D`** | App Launcher | Spawns configured menu / application launcher |
| **`Mod + Q`** | Close Window | Closes currently focused window |
| **`Mod + F`** | Fullscreen | Toggles fullscreen mode for active window |
| **`Mod + Space`** | Toggle Floating | Switches window between tiling and floating state |
| **`Mod + H`** / **`Left`** | Focus Left / Previous | Navigates focus to previous window |
| **`Mod + L`** / **`Right`** | Focus Right / Next | Navigates focus to next window |
| **`Mod + J`** / **`Down`** | Focus Down | Moves focus downwards in stack |
| **`Mod + K`** / **`Up`** | Focus Up | Moves focus upwards in stack |
| **`Mod + Shift + H/J/K/L`** | Swap Window | Reorders focused window position in tiling stack |
| **`Mod + 1` .. **`Mod + 9`** | Switch Workspace | Switches to workspace 1–9 |
| **`Mod + Shift + 1..9`** | Move to Workspace | Sends active window to workspace 1–9 |
| **`Mod + Shift + E`** | Exit Compositor | Shuts down `termuxwc` session cleanly |
| **`Mod + Left Drag`** | Move Floating | Drags a floating window |
| **`Mod + Right Drag`** | Resize Floating | Resizes a floating window |
| **`Left Click`** | Focus Window | Raises and focuses clicked window |

---

## Configuration

Configuration is located at `~/.config/termuxwc/config` and created automatically on first run:

```ini
# termuxwc Configuration

# Modifier key: Mod4 (Super), Mod1 (Alt), or ModBoth (Both Alt & Super)
mod ModBoth

# Default applications
terminal foot
launcher foot -e fzf

# Geometry & Gaps (in pixels)
inner_gap 6
outer_gap 8
border_width 2

# Colors (Hex format: #RRGGBB)
active_border #7aa2f7
inactive_border #3b4261
background #16161e
```

---

## Hardware GPU Acceleration

`termuxwc` includes a built-in hardware acceleration supervisor in `startup.sh` that detects your hardware on startup:

```
[ Termux GUI Applications / Games / Video Players ]
                       │
             (GALLIUM_DRIVER=virpipe)
                       ▼
            virgl_test_server_android
                       │
       ┌───────────────┴───────────────┐
       ▼                               ▼
 [ ARM Mali GPUs ]              [ Other Mobile GPUs ]
 ANGLE over Android Host        Android Vendor GLES
 Vulkan 1.3 (Mali-G68)          Driver (Adreno / PowerVR)
       │                               │
       └───────────────┬───────────────┘
                       ▼
        Physical GPU Silicon Hardware
```

| GPU Architecture | Node / Detection | OpenGL ES Acceleration | Vulkan Driver |
| :--- | :--- | :--- | :--- |
| **Qualcomm Adreno** | `/dev/kgsl-3d0` | Host GLES via VirGL | Turnip (`mesa-vulkan-icd-freedreno`) Direct Hardware |
| **ARM Mali** | `/dev/mali0` | Host Vulkan via VirGL + ANGLE | Lavapipe (`mesa-vulkan-icd-swrast`) Multi-threaded CPU |
| **PowerVR / Rogue** | `/dev/pvr_sync` | Host GLES via VirGL | Lavapipe (`mesa-vulkan-icd-swrast`) Multi-threaded CPU |
| **Samsung Xclipse** | `/dev/dri/renderD128` | Host AMD GLES via VirGL | Lavapipe (`mesa-vulkan-icd-swrast`) Multi-threaded CPU |
| **Linux Direct DRM** | `/dev/dri/renderD128` | Direct Mesa DRM / GBM | Direct Mesa Vulkan ICD |
| **Fallback (CPU)** | Generic CPU | Multi-threaded Mesa `llvmpipe` | Lavapipe (`mesa-vulkan-icd-swrast`) Multi-threaded CPU |

> [!TIP]
> **ARM Mali Hardware Acceleration**: ARM Mali GPUs on Android do not have an open-source Linux kernel Vulkan ICD like Qualcomm's Turnip. Standard VirGL GLES crashes on Mali due to restricted Android native sync fences. `termuxwc` solves this by automatically routing VirGL through ANGLE backed by the Android host Vulkan 1.3 driver (`--angle-vulkan`), providing reliable 3D GPU hardware acceleration without crashes.

### High-Refresh Rate (120+ FPS Headless Frame Clock)
`termuxwc` features a high-precision internal frame timer driving wlroots scene commits and client frame callbacks at up to 120 Hz, eliminating offscreen stalls in headless environments.
Set your preferred target FPS when starting the session:
```bash
TERMUXWC_FPS=120 termuxwc-session
```

### Complete Graphical Environment & Session Profile
`termuxwc-session` initializes a complete Linux graphical environment including a D-Bus session bus (`dbus-launch`), standard toolkit bridges (`GDK_BACKEND=wayland`, `QT_QPA_PLATFORM=wayland`, `SDL_VIDEODRIVER=wayland`), XDG directories, and GPU variables. 

All session environment variables are automatically written to `~/.config/termuxwc/env` on session start and can be sourced anywhere:
```bash
source ~/.config/termuxwc/env
```

### Pure Wayland & WayVNC Streaming Architecture
`termuxwc` is a pure native Wayland compositor. It operates completely independently of any X11 server — no X11 is used or supported anywhere in the pipeline:
* **Compositor**: Headless `termuxwc -H -r 120` running natively in Termux userspace.
* **Streamer**: `wayvnc` streaming directly over `0.0.0.0:5900` at full 120 FPS.
* **Clients**: Connect from any VNC client (e.g. [AVNC](https://github.com/gstansev/AVNC) on Android at `127.0.0.1:5900` or remotely over LAN).

---

## GPU Acceleration Without VNC (In-App Direct Rendering)

The VirGL bridge described above accelerates **client applications** (`foot`, games, video players) no matter how pixels reach the screen - it works identically with VNC, nested Wayland, or DRM. If you want the **compositor itself** to render on the physical GPU and display **without any VNC step**, there are three paths, ordered by speed:

### Path 1 - Native DRM backend (bare-metal Linux, zero overhead)
On real Linux (or Termux with root and an accessible `/dev/dri/renderD128`), wlroots' DRM backend makes termuxwc a KMS master: direct GBM/EGL rendering on the GPU, page-flip presentation, no capture, no encoding, no network.

```bash
# Run termuxwc directly on a free VT (no other session holding the display):
sudo termuxwc            # autocreate picks DRM automatically
# Force it explicitly:
termuxwc -b drm
```
Note: `-b drm` is compiled in whenever wlroots exposes `wlr/backend/drm.h` (standard on Linux; absent in the Termux Android build). On Termux without root this option falls back to headless with a log message.

Requirements: `/dev/dri/renderD128` readable (add the user to `video`/`render` groups or run as root), and no other compositor holding the DRM device.

### Path 2 - Nested Wayland backend (no root, no VNC, GPU-accelerated)
> **Important:** This project does **not** support X11 in any form — there is no X11 backend, and Termux:X11 (an X11 server) cannot be used anywhere in the pipeline. Everything is pure Wayland end-to-end. The nested path requires a real **Wayland** display host as parent. Options, best-first:

| Parent display host | What it is | How to get it |
| :--- | :--- | :--- |
| **[Weston](https://gitlab.freedesktop.org/wlroots/weston)** *(recommended)* | Reference Wayland compositor; runs on DRM, headless, or nested under another Wayland session | `pkg install weston` in Termux / distro package on Linux |
| **Cage** | Single-window kiosk wlroots compositor, runs on DRM or nested Wayland | `pkg install cage` (if available) / build from source |
| **Your own app (SurfaceView)** | Embed a tiny wlroots/XCB parent compositor in your Android app (*Waydroid-shell* style), expose `WAYLAND_DISPLAY` | Custom dev — see Path 3 for the lighter dmabuf variant |
| **GNOME / KDE / wlroots desktop** | Any native Wayland session on a Linux PC (or chroot/Proot distro) | Log into a Wayland session and run termuxwc inside it |

**Recipe A — Native Wayland session on Linux (the standard no-root, no-VNC setup):**
Log into any Wayland desktop (GNOME/KDE/sway/Weston session) and run termuxwc inside it — it opens as a normal Wayland window:
```bash
# Inside a running Wayland session (WAYLAND_DISPLAY is already exported):
TERMUXWC_BACKEND=wayland TERMUXWC_RENDERER=gles2 termuxwc-session
# or directly:
termuxwc -b wayland -d weston-0 -R gles2   # -d: parent display (or export WAYLAND_DISPLAY)
```
Clients launched from termuxwc's terminals stay inside termuxwc; termuxwc's frames go through the parent compositor's GLES path straight to the screen — GPU-composited presentation, no VNC codec step. (`TERMUXWC_BACKEND=wayland` refuses to start with a helpful "display parameter is not set" message if neither `-d <display>` nor a parent `WAYLAND_DISPLAY`/`TERMUXWC_DISPLAY` is given.)

**Recipe B — Weston on DRM (rooted device or Linux console) + termuxwc nested inside it:**
```bash
weston --backend=headless &      # or --backend=drm on rooted devices
WAYLAND_DISPLAY=weston-0 termuxwc -b wayland -R gles2   # or: termuxwc -b wayland -d weston-0
```

**Recipe C — embed a minimal wlroots parent in your own app:**
Ship a tiny wlroots-based host compositor (Weston/Cage work out of the box) inside the app process space, expose its socket via `WAYLAND_DISPLAY`, then launch `termuxwc -b wayland` against it. No X11 layer exists anywhere in this chain.

In all cases add `GALLIUM_DRIVER=virpipe` (or the Turnip/ANGLE env from the VirGL section) so client apps keep GPU acceleration through the bridge.

### Path 3 - Zero-copy dmabuf capture for your own viewer app
When pixels must be shipped into your own Android application (SurfaceView) without VNC, use the `zwlr_screencopy_manager_v1` global that termuxwc already exposes together with `zwp_linux_dmabuf_v1` (created from the GLES renderer at startup). With `-R gles2`, captured buffers are exported as **dmabufs** (AHardwareBuffer-backed on Adreno/Mali under Termux), so a small screencopy client inside your app can import them with `EGL_EXT_image_dma_buf_import` and blit straight onto the `NativeWindow` - one GPU-side copy, no JPEG/H.264 encode round-trip like wayvnc performs. This requires writing a few hundred lines of capture code in the app; the compositor side needs no changes.

### Enabling the GLES2 renderer for the compositor headlessly
By default `startup.sh` uses `WLR_RENDERER=pixman` (CPU) for maximum stability. To make the compositor's own scene graph render on the GPU through the VirGL bridge:

```bash
TERMUXWC_RENDERER=gles2 termuxwc-session
```
This sets `WLR_RENDERER=gles2`; if EGL/GLES initialization fails (bridge not running), termuxwc falls back to Pixman automatically - sessions never fail to start because of renderer choice. Verify with:
```bash
termuxwc-gpu-check          # shows active renderer and EGL strings
glxinfo | grep -i opengl    # should report "virgl" / Turnip / ANGLE
```

| Setup | Compositor renderer | Presentation | Latency | Root needed |
| :--- | :--- | :--- | :--- | :--- |
| Headless + wayvnc (default) | pixman (CPU) | VNC TCP stream | ~30-60 ms | No |
| Headless + gles2 + wayvnc | gles2 (VirGL -> GPU) | VNC TCP stream | ~20-40 ms | No |
| Nested Wayland (`-b wayland`) | gles2 | Parent Wayland compositor | ~10 ms | No |
| DRM (`-b drm`) | gles2 (GBM) | Page flip | <5 ms | Yes/groups |
| Screencopy dmabuf -> SurfaceView | gles2 | Zero-copy import | ~5-10 ms | No |

### GPU Diagnostics & 3D Benchmark Tools
`termuxwc` includes built-in diagnostic and benchmarking tools:
```bash
# Full GPU driver stack and bridge diagnostic check
termuxwc-gpu-check

# Real-time native Wayland 3D GPU accelerated cube benchmark
termuxwc-cube
```
This utility:
- Detects the physical SoC and GPU silicon (`/dev/mali0`, `/dev/kgsl-3d0`, `/dev/dri/*`).
- Validates the active VirGL hardware 3D bridge (`virpipe` / ANGLE-Vulkan).
- Verifies native Wayland EGL hardware acceleration on Mali-G68 / Adreno.
- Measures real-time FPS and buffer swap latencies.

To run any application with GPU acceleration:
```bash
GALLIUM_DRIVER=virpipe <application>
```

---

## Building from Source

If you prefer to compile `termuxwc` manually:

### 1. Install Dependencies

```bash
pkg update
pkg install clang meson ninja pkg-config libwayland wayland-protocols \
            libxkbcommon libpixman wayvnc foot virglrenderer-android
```

### 2. Patch & Build wlroots 0.19.1

> [!NOTE]
> Android's Bionic C library lacks POSIX `shm_open()`. Upstream wlroots fails without patching `util/shm.c` to use `mkostemp()` in `$XDG_RUNTIME_DIR`.

```bash
git clone https://gitlab.freedesktop.org/wlroots/wlroots.git -b 0.19.1
cd wlroots
```

In `util/shm.c`, replace `allocate_shm_file_pair` with an Android `mkostemp` implementation:

```c
#ifdef __ANDROID__
#include <stdlib.h>
#include <limits.h>
#include <unistd.h>
#include <fcntl.h>

static void get_shm_dir(char *buf, size_t len) {
    const char *dir = getenv("XDG_RUNTIME_DIR");
    if (!dir || access(dir, W_OK) != 0) dir = "/data/data/com.termux/files/usr/tmp";
    snprintf(buf, len, "%s", dir);
}
#endif
```

Build and install wlroots:
```bash
meson setup build --prefix="$PREFIX" -Dexamples=false
ninja -C build install
```

### 3. Build termuxwc

```bash
git clone https://github.com/Oflucoder/termuxwc.git
cd termuxwc
meson setup build --prefix="$PREFIX"
ninja -C build
ninja -C build install
```

---

## Testing

### Automated smoke test (no display, no VNC, no X11 required)

```bash
./tools/smoke-test.sh ./build/termuxwc
```

The script verifies, entirely headlessly:
1. **Boot** — compositor starts with `-b headless`, creates its Wayland socket within 3 s and stays alive.
2. **Protocol handshake** — `weston-info` / `wayland-info` binds and sees `wl_compositor`, `xdg_wm_base`, `wlr_layer_shell_v1` (and the `zwlr_screencopy_manager_v1` dmabuf capture path when present).
3. **Client spawn** — a real Wayland app (`foot` or `weston-simple-egl`) is launched via `-s` and the compositor survives map/commit cycles.

Install helpers for full coverage: `apt install weston-utils wayland-utils foot` (Linux) or `pkg install weston-utils foot` (Termux). The script exits non-zero on any failure, so it can be wired into CI.

### Manual test matrix

| What | How to test | Expected result |
| :--- | :--- | :--- |
| Headless + capture | `termuxwc -b headless &` then run your viewer against `$WAYLAND_DISPLAY` | Socket appears, frames commit only on scene change (`needs_frame` optimization) |
| Nested Wayland (GPU, no root/VNC) | `weston --backend=headless &` → `WAYLAND_DISPLAY=weston-0 termuxwc -b wayland -R gles2` (equivalently `termuxwc -b wayland -d weston-0`) | termuxwc window shown by parent compositor, EGL/GLES2 active in log |
| Native DRM (real GPU page flips) | Linux console session free of other compositors: `termuxwc -b drm` | KMS output lights up directly, no VNC/X11 involved |
| Frame pump idle cost | `termuxwc -b headless &`, open no windows, `top -p $(pidof termuxwc)` | CPU ~0 % when nothing changes on screen |
| Tiling / keybindings | Launch `foot`, Super+Enter spawns terminal, Super+j/k cycle tiles | Windows arrange without gaps |

## Command-Line Reference

```
Usage: termuxwc [-s <cmd>] [-w <width>] [-h <height>] [-r <fps>] [-R <renderer>]
                [-b <auto|drm|wayland|headless>] [-d <display>]
```

| Option | Description | Default |
| :--- | :--- | :--- |
| `-s <cmd>` | Startup command executed once compositor is ready | `None` |
| `-w <width>` | Virtual display width in pixels | `1280` |
| `-h <height>` | Virtual display height in pixels | `720` |
| `-r <fps>` | Frame clock rate for headless/nested outputs | `60` |
| `-b <backend>` | Backend: `auto`, `drm`, `wayland`, `headless` (no X11) | `auto` |
| `-R <renderer>` | Compositor renderer (`auto`, `gles2`, `vulkan`, `pixman`) | `auto` |
| `-d <display>` | **Parent Wayland display for `-b wayland`** — a display name (`weston-0`) or a full socket path (`/dev/shm/wayland-1`). Required when `WAYLAND_DISPLAY` is not exported; without it termuxwc exits with "display parameter is not set" instead of silently falling back. | `$WAYLAND_DISPLAY` |

> Note: `-d` refers to the *parent* compositor's display that termuxwc connects to as a client (nested mode). termuxwc's own socket is still auto-created and exported to its children as `WAYLAND_DISPLAY`.

---

## License

This project is licensed under the [MIT License](LICENSE).
