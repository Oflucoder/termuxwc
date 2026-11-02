#!/data/data/com.termux/files/usr/bin/bash
# termuxwc Installer — Termux (Android, rootless) and native Linux.
#
#   Termux:  bash install.sh          (pkg deps + build from source)
#   Linux :  sudo ./install.sh        (apt/dnf deps + meson/ninja install)
#
# wayland-protocols is NOT fetched from the internet anymore: the required
# protocol C sources are vendored under include/protocols/, so a plain git
# clone is self-sufficient.
set -e

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
IS_TERMUX=0
case "$PREFIX" in
  /data/data/com.termux/*) IS_TERMUX=1 ;;
esac

echo "=========================================================="
if [ "$IS_TERMUX" = 1 ]; then
    echo "  termuxwc installer — Termux/Android"
    echo "=========================================================="

    # --- Step 1: dependencies (all available in Termux repos) ---
    echo ">> Step 1/3: Installing packages..."
    pkg update -y
    pkg install -y \
        clang make cmake ninja meson pkg-config \
        libwayland wayland-protocols wayland-utils \
        libxkbcommon libpixman pixman grapheme-breaker \
        libevicti libepoll-shim xmlto docbook-xsl \
        mesa virglrenderer-android curl tar dbus git \
        wayvnc foot

    # wlroots 0.19 is not in Termux repos as a standalone package; it ships
    # inside the 'termux-wlroots' meta or must be built. Try repo first,
    # fall back to building it from upstream sources into $PREFIX.
    if ! pkg-config --exists wlroots-0.19 2>/dev/null; then
        echo ">> wlroots-0.19 not found in repos; building from source..."
        TMP=$(mktemp -d); trap 'rm -rf "$TMP"' EXIT
        git clone --depth 1 --branch 0.19.0 \
            https://gitlab.freedesktop.org/wlroots/wlroots.git "$TMP/wlroots"
        ( cd "$TMP/wlroots" \
          && meson setup build --prefix="$PREFIX" \
               -Dexamples=false -Dbackends=headless,wayland,drm \
               -Drenderers=gles2,pixman \
          && ninja -C build && ninja -C build install )
    fi

    # --- Step 2: build termuxwc ---
    echo ">> Step 2/3: Building termuxwc from this checkout..."
    cd "$SCRIPT_DIR"
    rm -rf build
    meson setup build --prefix="$PREFIX"
    ninja -C build

    # --- Step 3: deploy session script + config ---
    echo ">> Step 3/3: Deploying termuxwc-session and default config..."
    cp -f startup.sh "$PREFIX/bin/termuxwc-session"
    chmod 755 "$PREFIX/bin/termuxwc-session"
    [ -f build/termuxwc-cube ] && { cp -f build/termuxwc-cube "$PREFIX/bin/"; chmod 755 "$PREFIX/bin/termuxwc-cube"; }
    mkdir -p "$HOME/.config/termuxwc"
    [ -f "$HOME/.config/termuxwc/config" ] || cp "$SCRIPT_DIR/config" "$HOME/.config/termuxwc/config"

else
    echo "  termuxwc installer — Native Linux"
    echo "=========================================================="

    # --- Step 1: distro dependencies ---
    echo ">> Step 1/3: Installing system packages..."
    if command -v apt-get >/dev/null; then
        sudo apt-get update
        sudo apt-get install -y build-essential meson ninja-build pkg-config \
            libwayland-dev wayland-protocols libwlroots-0.19-dev \
            libxkbcommon-dev libpixman-1-dev libegl1-mesa-dev \
            libgles2-mesa-dev libpng-dev dbus wayland-utils
    elif command -v dnf >/dev/null; then
        sudo dnf install -y gcc meson ninja-build pkgconf-pkg-config \
            wayland-devel wayland-protocols-devel wlroots-devel \
            libxkbcommon-devel pixman-devel mesa-libEGL-devel \
            mesa-libGLES-devel libpng-devel dbus-devel
    else
        echo "WARNING: unknown package manager; ensure wlroots-0.19 dev files exist."
    fi

    # --- Step 2: build & install ---
    echo ">> Step 2/3: Building and installing to /usr/local..."
    cd "$SCRIPT_DIR"
    rm -rf build
    meson setup build --prefix=/usr/local
    ninja -C build
    sudo ninja -C build install

    # --- Step 3: session script + config ---
    echo ">> Step 3/3: Deploying termuxwc-session and default config..."
    sudo cp -f startup.sh /usr/local/bin/termuxwc-session
    sudo chmod 755 /usr/local/bin/termuxwc-session
    mkdir -p "$HOME/.config/termuxwc"
    [ -f "$HOME/.config/termuxwc/config" ] || cp "$SCRIPT_DIR/config" "$HOME/.config/termuxwc/config"
fi

echo ""
echo "=========================================================="
echo "           INSTALLATION FINISHED 🎉"
echo "=========================================================="
echo ""
echo "Start a session:"
echo "    termuxwc-session                          # headless (+VNC on Android)"
echo "    TERMUXWC_BACKEND=termux-api termuxwc-session   # embedded, no VNC (Termux:API)"
echo "    TERMUXWC_BACKEND=drm termuxwc-session     # native Linux console, no VNC"
echo "    TERMUXWC_BACKEND=wayland TERMUXWC_DISPLAY=wayland-0 termuxwc-session"
echo "                                              # nested window, no VNC"
echo ""
