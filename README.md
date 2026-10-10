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
                       ▼
             Android Vendor GLES Driver
                       ▼
       Physical GPU (Adreno / Mali / PowerVR)
```

| GPU Architecture | Node / Detection | Driver Stack |
| :--- | :--- | :--- |
| **Qualcomm Adreno** | `/dev/kgsl-3d0` | Host Vendor GLES via VirGL / Turnip + Zink |
| **ARM Mali** | `/dev/mali0` | Host Vendor GLES (`libGLES_mali.so`) via VirGL |
| **PowerVR / Rogue** | `/dev/pvr_sync` | Host Vendor GLES (`libGLES_pvr.so`) via VirGL |
| **Samsung Xclipse** | `/dev/dri/renderD128` | Host AMD GLES via VirGL |
| **Linux Direct DRM** | `/dev/dri/renderD128` | Direct Mesa DRM / GBM pass-through |
| **Fallback** | Generic CPU | Multi-threaded Mesa `llvmpipe` |

To run any application with GPU acceleration manually:
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

## Command-Line Reference

```
Usage: termuxwc [-s <cmd>] [-w <width>] [-h <height>] [-H] [-R <renderer>]
```

| Option | Description | Default |
| :--- | :--- | :--- |
| `-s <cmd>` | Startup command executed once compositor is ready | `None` |
| `-w <width>` | Virtual display width in pixels | `1280` |
| `-h <height>` | Virtual display height in pixels | `720` |
| `-H` | Force headless backend mode | Auto-detect |
| `-R <renderer>` | Compositor renderer (`auto`, `gles2`, `vulkan`, `pixman`) | `auto` |

---

## License

This project is licensed under the [MIT License](LICENSE).
