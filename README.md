# termuxwc: SwayWM-Grade Tiling Wayland Compositor for Termux

A native, high-performance **SwayWM-grade tiling Wayland compositor** built from scratch with **wlroots-0.19**, tailored specifically for **rootless Android (Termux)** and Linux mobile environments.

`termuxwc` brings the full power, workflow, and aesthetics of a modern tiling window manager (dynamic master-stack tiling, 9 virtual workspaces, inner/outer gaps, active/inactive window borders, and Sway keybindings) directly to your Android device without requiring root privileges or hardware GPU acceleration, streaming over ultra-low-latency VNC via `wayvnc`.

---

## Key Features

- **Rootless & 100% Native**: Runs entirely inside Termux userspace with no proot overhead, no root access, and no kernel modifications required.
- **Dynamic Auto-Tiling Engine**:
  - Master-stack tiling that automatically adapts to your phone or tablet orientation.
  - **Landscape Mode**: Splits master on the left with vertical stacks on the right.
  - **Portrait Mode**: Splits master on top with horizontal stacks on the bottom (ideal for one-handed mobile usage).
- **9 Virtual Workspaces**:
  - Instant workspace switching (`Mod + 1..9`).
  - Move individual windows between workspaces (`Mod + Shift + 1..9`).
  - Isolated scene graphs per workspace so hidden windows consume zero rendering overhead.
- **Modern Aesthetics & Gaps**:
  - Configurable inner and outer gaps (`inner_gap`, `outer_gap`).
  - Custom border width and colors for active and inactive windows (`active_border`, `inactive_border`).
  - Custom wallpaper / solid background color canvas.
- **Full SwayWM Keybinding Suite**:
  - Familiar i3 / Sway shortcuts (`Mod+Return`, `Mod+Q`, `Mod+F`, `Mod+Space`, `Mod+H/J/K/L`, `Mod+1..9`, `Mod+Shift+E`).
  - **Mobile Mod Key Support (`ModBoth`)**: Allows **Alt** and **Super/Windows** keys to work interchangeably out of the box—essential for Android on-screen keyboards (such as Gboard or Hacker's Keyboard) which rarely have a physical Super key.
- **Extensible Configuration System**:
  - Simple, human-readable config at `~/.config/termuxwc/config`.
  - Automatically generated with sleek defaults on first launch.
- **Desktop Protocol Support**:
  - **XDG-Shell** (`xdg_wm_base`): Modern application window lifecycle, interactive moves and resizes, fullscreen and floating toggles.
  - **Layer-Shell** (`zwlr_layer_shell_v1`): Support for status bars (`waybar`), wallpapers (`swaybg`, `wpaperd`), and desktop notifications.
  - **Virtual Input**: Full integration with `wlr-virtual-keyboard-v1` and `wlr-virtual-pointer-v1` for remote keyboard and pointer streaming.
  - **Screencopy**: Protocol support for `wayvnc` streaming and `grim` screenshots.
- **Universal GPU Hardware Acceleration Subsystem**:
  - Automatically identifies any mobile GPU architecture:
    - **Qualcomm Snapdragon** (Adreno 5xx, 6xx, 7xx, 8xx via `/dev/kgsl-3d0`)
    - **ARM Mali** (Samsung Exynos, MediaTek Dimensity/Helio, Google Tensor, HiSilicon Kirin via `/dev/mali*`)
    - **PowerVR / Rogue** (Unisoc, Rockchip via `/dev/pvr*`)
    - **Samsung / AMD Xclipse** (RDNA2/RDNA3)
    - **Linux Direct DRM Nodes** (`/dev/dri/renderD128`)
  - Integrated VirGL hardware 3D bridge (`virgl_test_server_android --multi-clients`) offloads OpenGL/GLES calls directly to the host physical GPU (`GALLIUM_DRIVER=virpipe`).
  - Pluggable compositor rendering (`-R auto|gles2|vulkan|pixman`) with automatic, graceful Pixman fallback.

---

## Technical Challenges Solved

### 1. Android Bionic Shared Memory (`shm_open`)
* **Problem**: Android's Bionic C library does not implement POSIX `shm_open()` (returns `ENOSYS`). In upstream wlroots, `util/shm.c` fails when allocating shared memory file pairs (`allocate_shm_file_pair`), preventing keymaps from reaching client applications. This caused clients like `foot` to immediately terminate with `Broken pipe`.
* **Solution**: Patched `wlroots/util/shm.c` to use `mkostemp()` inside `$XDG_RUNTIME_DIR` or `/tmp` to create temporary file descriptors that work reliably across rootless Android processes.

### 2. VNC Virtual Input Event Routing
* **Problem**: `wayvnc` injects remote touch/mouse clicks and keystrokes using the `wlr-virtual-keyboard-v1` and `wlr-virtual-pointer-v1` protocols. These do not pass through the backend's standard hardware input signal, causing the terminal to ignore all keyboard typing and mouse clicks.
* **Solution**: Wired up `wlr_virtual_keyboard_manager_v1` and `wlr_virtual_pointer_manager_v1` listeners, attaching virtual pointers to `wlr_cursor` and virtual keyboards to `wlr_seat`, routing keyboard enter events directly to active windows.

---

## Configuration (`~/.config/termuxwc/config`)

On first launch, `termuxwc` automatically creates `~/.config/termuxwc/config` if it does not already exist:

```ini
# termuxwc Sway-grade Tiling Window Manager Configuration

# Modifier key:
# Mod4    = Super / Windows key
# Mod1    = Alt key
# ModBoth = Both Alt and Super work interchangeably (Recommended for mobile)
mod ModBoth

# Default applications
terminal foot
launcher foot -e fzf

# Geometry & Gaps (in pixels)
inner_gap 6
outer_gap 8
border_width 2

# Colors (Hex format #rrggbb)
active_border #7aa2f7
inactive_border #3b4261
background #16161e
```

---

## Keybindings Reference

| Shortcut | Action | Description |
| :--- | :--- | :--- |
| **`Mod + Return`** | Launch Terminal | Opens configured terminal (`foot`) |
| **`Mod + D`** | App Launcher | Opens configured application menu / launcher |
| **`Mod + Q`** | Close Window | Sends close request to currently focused window |
| **`Mod + F`** | Toggle Fullscreen | Expands active window across entire usable screen |
| **`Mod + Space`** | Toggle Floating | Switches window between tiling tree and floating state |
| **`Mod + H`** / **`Left`** | Focus Left / Prev | Moves focus to the previous tiled window |
| **`Mod + L`** / **`Right`** | Focus Right / Next | Moves focus to the next tiled window |
| **`Mod + J`** / **`Down`** | Focus Down | Moves focus downwards in stacking layout |
| **`Mod + K`** / **`Up`** | Focus Up | Moves focus upwards in stacking layout |
| **`Mod + Shift + H/J/K/L`** | Swap Window | Reorders focused window position within tiling layout |
| **`Mod + 1` .. **`Mod + 9`** | Switch Workspace | Switches active display to workspace 1 through 9 |
| **`Mod + Shift + 1..9`** | Move to Workspace | Sends focused window to specified workspace |
| **`Mod + Shift + E`** | Exit Compositor | Cleanly shuts down `termuxwc` session |
| **`Mod + Left Drag`** | Move Floating | Interactively drags a floating window |
| **`Mod + Right Drag`** | Resize Floating | Interactively resizes a floating window |
| **`Left Click`** | Focus Window | Clicks to focus and brings window to foreground |

> [!TIP]
> With `mod ModBoth`, both the **Alt** key and **Super (Windows)** key function as the `Mod` key!

---

## Installation & Setup

### 1. Install Dependencies in Termux

```bash
pkg update
pkg install clang meson ninja pkg-config libwayland wayland-protocols \
            libxkbcommon libpixman wayvnc foot
```

### 2. Patch & Install wlroots 0.19.1

If compiling wlroots from source:
```bash
git clone https://gitlab.freedesktop.org/wlroots/wlroots.git -b 0.19.1
cd wlroots
```

In `util/shm.c`, update `allocate_shm_file_pair` for Android:
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

### 3. Build & Install termuxwc

```bash
git clone https://github.com/yourusername/termuxwc.git
cd termuxwc

meson setup build --prefix="$PREFIX"
ninja -C build
ninja -C build install
```

---

## Running the Desktop Session

### Method 1: Automated Session Supervisor (Recommended)

Simply execute:

```bash
./startup.sh
```

`startup.sh` automatically:
1. Reaps any stale sockets or zombie processes (`termuxwc`, `wayvnc`, `foot`).
2. Re-compiles `termuxwc` if any changes exist.
3. Spawns `termuxwc` in headless mode (default 1280x720).
4. Launches `wayvnc` on `0.0.0.0:5900`.
5. Spawns the `foot` terminal inside the Wayland session.

To terminate the entire desktop session cleanly, press `Ctrl + C`.

### Method 2: Manual Startup

```bash
export XDG_RUNTIME_DIR="/data/data/com.termux/files/usr/tmp/xdg-runtime"
mkdir -p "$XDG_RUNTIME_DIR" && chmod 700 "$XDG_RUNTIME_DIR"

# Launch termuxwc with custom resolution and startup terminal
termuxwc -w 1920 -h 1080 -s foot
```

#### Command-Line Arguments:
| Flag | Description | Default |
| :--- | :--- | :--- |
| `-w <width>` | Virtual display width in pixels | `1280` |
| `-h <height>` | Virtual display height in pixels | `720` |
| `-s <command>` | Initial command to spawn on startup | `None` |
| `-H` | Force headless backend mode | Auto |
| `-R <renderer>` | Compositor renderer (`auto`, `gles2`, `vulkan`, `pixman`) | `auto` |

---

## Connecting via VNC

Once `termuxwc` and `wayvnc` are running:

1. Open your VNC client:
   - **Android Device**: [AVNC](https://github.com/gstansev/AVNC) (highly recommended for touch & pointer controls) or [bVNC](https://github.com/bk138/multivnc).
   - **External PC**: TigerVNC, RealVNC Viewer, or Remmina.
2. Enter connection details:
   - **Local Phone**: `127.0.0.1:5900`
   - **Over Wi-Fi / LAN**: `<phone-ip>:5900`
3. Hit Connect (no password required). You will see your tiled Sway desktop ready for multitasking!

---

## Project Structure

```
termuxwc/
├── include/
│   ├── protocols/        # Generated Wayland server protocol headers & sources
│   │   ├── wlr-layer-shell-unstable-v1-protocol.h
│   │   ├── wlr-layer-shell-unstable-v1-protocol.c
│   │   ├── xdg-shell-protocol.h
│   │   └── xdg-shell-protocol.c
│   └── termuxwc/
│       └── server.h      # Core structs: workspaces, config, toplevels, scene trees
├── src/
│   └── main.c           # Dynamic auto-tiling WM engine, Sway keybindings & event loops
├── meson.build          # Meson build file with pixman, xkbcommon, wlroots dependencies
├── startup.sh           # Session launcher & process manager
└── README.md            # Comprehensive documentation
```

---

## License

MIT License. Feel free to use, modify, and contribute back!
