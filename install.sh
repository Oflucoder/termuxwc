#!/data/data/com.termux/files/usr/bin/bash
# termuxwc One-Click Installer for Termux (Android rootless)
set -e

echo "=========================================================="
echo "    termuxwc: SwayWM Tiling Compositor Installer"
echo "=========================================================="

PREFIX="${PREFIX:-/data/data/com.termux/files/usr}"
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# 1. Install all required dependencies from Termux mirrors
echo ">> Step 1/4: Installing required packages (Wayland, pixman, xkb, wayvnc, foot, virgl)..."
pkg update -y
pkg install -y \
    libwayland wayland-protocols \
    libxkbcommon libpixman \
    wayvnc foot virglrenderer-android curl tar

# 2. Deploy prebuilt patched wlroots library and termuxwc binary
echo ">> Step 2/4: Deploying precompiled binaries..."
if [ -f "$SCRIPT_DIR/libwlroots-0.19.so" ] && [ -f "$SCRIPT_DIR/termuxwc" ]; then
    # Local release bundle
    cp -f "$SCRIPT_DIR/libwlroots-0.19.so" "$PREFIX/lib/"
    cp -f "$SCRIPT_DIR/termuxwc" "$PREFIX/bin/"
elif [ -f "$SCRIPT_DIR/dist/termuxwc-v0.4-aarch64/libwlroots-0.19.so" ]; then
    # Cloned repo with dist folder
    cp -f "$SCRIPT_DIR/dist/termuxwc-v0.4-aarch64/libwlroots-0.19.so" "$PREFIX/lib/"
    cp -f "$SCRIPT_DIR/dist/termuxwc-v0.4-aarch64/termuxwc" "$PREFIX/bin/"
else
    # Download prebuilt tarball from GitHub Release
    echo ">> Fetching prebuilt release package from GitHub..."
    TMP_DIR=$(mktemp -d)
    TAR_URL="https://github.com/Oflucoder/termuxwc/releases/download/v0.4/termuxwc-v0.4-aarch64.tar.gz"
    if curl -sLf "$TAR_URL" -o "$TMP_DIR/release.tar.gz"; then
        tar -xzf "$TMP_DIR/release.tar.gz" -C "$TMP_DIR"
        cp -f "$TMP_DIR/libwlroots-0.19.so" "$PREFIX/lib/"
        cp -f "$TMP_DIR/termuxwc" "$PREFIX/bin/"
        rm -rf "$TMP_DIR"
    elif [ -f "$SCRIPT_DIR/build/termuxwc" ]; then
        cp -f "$SCRIPT_DIR/build/termuxwc" "$PREFIX/bin/"
    fi
fi

chmod 755 "$PREFIX/lib/libwlroots-0.19.so" 2>/dev/null || true
chmod 755 "$PREFIX/bin/termuxwc" 2>/dev/null || true

# 3. Deploy session supervisor script (termuxwc-session)
echo ">> Step 3/4: Setting up session supervisor command (termuxwc-session)..."
if [ -f "$SCRIPT_DIR/startup.sh" ]; then
    cp -f "$SCRIPT_DIR/startup.sh" "$PREFIX/bin/termuxwc-session"
elif [ -f "$SCRIPT_DIR/dist/termuxwc-v0.4-aarch64/startup.sh" ]; then
    cp -f "$SCRIPT_DIR/dist/termuxwc-v0.4-aarch64/startup.sh" "$PREFIX/bin/termuxwc-session"
fi
chmod 755 "$PREFIX/bin/termuxwc-session"

# 4. Deploy default config
echo ">> Step 4/4: Initializing configuration (~/.config/termuxwc/config)..."
mkdir -p "$HOME/.config/termuxwc"
if [ ! -f "$HOME/.config/termuxwc/config" ]; then
    if [ -f "$SCRIPT_DIR/config" ]; then
        cp "$SCRIPT_DIR/config" "$HOME/.config/termuxwc/config"
    elif [ -f "$SCRIPT_DIR/dist/termuxwc-v0.4-aarch64/config" ]; then
        cp "$SCRIPT_DIR/dist/termuxwc-v0.4-aarch64/config" "$HOME/.config/termuxwc/config"
    fi
fi

echo ""
echo "=========================================================="
echo "           INSTALLATION FINISHED SUCCESSFULLY! 🎉"
echo "=========================================================="
echo ""
echo "To start your Sway-grade desktop session, simply run:"
echo "    termuxwc-session"
echo ""
echo "Then connect your VNC client (AVNC on Android recommended) to:"
echo "    127.0.0.1:5900"
echo ""
