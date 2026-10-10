#!/bin/bash

# --- termuxwc Universal Desktop & GPU Acceleration Supervisor ---
echo "=== termuxwc Desktop Session with Universal GPU Acceleration ==="

export XDG_RUNTIME_DIR="/data/data/com.termux/files/usr/tmp/xdg-runtime"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# Clean up any stale processes or lockfiles from previous runs
pkill -9 -f termuxwc &>/dev/null || true
pkill -9 -f wayvnc &>/dev/null || true
pkill -9 -f foot &>/dev/null || true
pkill -9 -f virgl_test_server_android &>/dev/null || true
rm -f "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/wayvnc* &>/dev/null || true
rm -f /data/data/com.termux/files/usr/tmp/.virgl_test &>/dev/null || true

LOG_FILE="termuxwc.log"
VNC_LOG="wayvnc.log"
FOOT_LOG="foot.log"
GPU_LOG="gpu.log"

COMPOSITOR_PID=
VNC_PID=
FOOT_PID=
VIRGL_PID=

cleanup() {
    echo ""
    echo "=== Shutting down termuxwc session ==="
    [ -n "$FOOT_PID" ] && kill "$FOOT_PID" &>/dev/null
    [ -n "$VNC_PID" ] && kill "$VNC_PID" &>/dev/null
    [ -n "$COMPOSITOR_PID" ] && kill "$COMPOSITOR_PID" &>/dev/null
    [ -n "$VIRGL_PID" ] && kill "$VIRGL_PID" &>/dev/null
    rm -f "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/wayvnc* &>/dev/null || true
    rm -f /data/data/com.termux/files/usr/tmp/.virgl_test &>/dev/null || true
    echo "All processes and GPU bridges stopped."
    exit 0
}
trap cleanup INT TERM EXIT

# --- Step 1: Universal Mobile GPU Auto-Detection ---
echo "Detecting Graphics Hardware..."

DETECTED_GPU="Unknown"
GPU_VENDOR="Unknown"
ACCEL_TYPE="CPU Software (llvmpipe)"

PLATFORM=$(getprop ro.board.platform 2>/dev/null || echo "")
HARDWARE=$(getprop ro.hardware 2>/dev/null || echo "")
SOC_MODEL=$(getprop ro.soc.model 2>/dev/null || echo "")

if [ -e /dev/kgsl-3d0 ] && [ -r /dev/kgsl-3d0 ]; then
    # Qualcomm Snapdragon (Adreno GPUs)
    GPU_VENDOR="Qualcomm"
    DETECTED_GPU="Adreno GPU (${SOC_MODEL:-$PLATFORM})"
    ACCEL_TYPE="Hardware Acceleration"
elif [ -e /dev/mali0 ] || [ -e /dev/mali ]; then
    # ARM Mali (Samsung Exynos, MediaTek Dimensity/Helio, Google Tensor, HiSilicon Kirin)
    GPU_VENDOR="ARM"
    DETECTED_GPU="Mali GPU (${SOC_MODEL:-$PLATFORM})"
    ACCEL_TYPE="Hardware Acceleration"
elif [ -e /dev/pvr_sync ] || [ -e /dev/pvrsrvkm ]; then
    # PowerVR (Unisoc, Rockchip)
    GPU_VENDOR="Imagination"
    DETECTED_GPU="PowerVR Rogue GPU (${SOC_MODEL:-$PLATFORM})"
    ACCEL_TYPE="Hardware Acceleration"
elif [ -e /dev/dri/renderD128 ] && [ -r /dev/dri/renderD128 ]; then
    # Linux Desktop Direct DRM / AMD / Intel
    GPU_VENDOR="DRM Direct"
    DETECTED_GPU="Linux Direct DRM Node (${SOC_MODEL:-$PLATFORM})"
    ACCEL_TYPE="Hardware Acceleration"
else
    GPU_VENDOR="CPU"
    DETECTED_GPU="Software Rasterizer (${PLATFORM:-Generic})"
    ACCEL_TYPE="Software (CPU)"
fi

echo ">> Detected Hardware: $DETECTED_GPU [Vendor: $GPU_VENDOR]"
echo ">> Target Profile:    $ACCEL_TYPE"

# --- Step 2: Initialize Universal GPU Acceleration Bridge ---
if [ -x "$(command -v virgl_test_server_android)" ]; then
    echo "Starting VirGL hardware GPU bridge server..."
    > "$GPU_LOG"
    virgl_test_server_android --multi-clients &> "$GPU_LOG" &
    VIRGL_PID=$!
    
    # Wait for the VirGL UNIX socket
    for i in $(seq 1 20); do
        if [ -S /data/data/com.termux/files/usr/tmp/.virgl_test ]; then
            break
        fi
        sleep 0.1
    done

    if kill -0 "$VIRGL_PID" &>/dev/null && [ -S /data/data/com.termux/files/usr/tmp/.virgl_test ]; then
        echo ">> VirGL Hardware 3D Bridge is ACTIVE on $DETECTED_GPU (PID $VIRGL_PID)"
        export GALLIUM_DRIVER=virpipe
        export MESA_GL_VERSION_OVERRIDE=3.3
        export MESA_GLSL_VERSION_OVERRIDE=330
        export VIRGL_CLIENT_DIR="/data/data/com.termux/files/usr/tmp"
    else
        echo "WARNING: VirGL bridge failed to start. Falling back to multi-threaded CPU Mesa."
        export LIBGL_ALWAYS_SOFTWARE=1
        export GALLIUM_DRIVER=llvmpipe
    fi
else
    echo "NOTE: virgl_test_server_android not installed; using standard Mesa driver."
fi

# --- Vulkan Graphics Driver Configuration ---
if [ "$GPU_VENDOR" = "Qualcomm" ] && [ -f /data/data/com.termux/files/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json ]; then
    export VK_ICD_FILENAMES="/data/data/com.termux/files/usr/share/vulkan/icd.d/freedreno_icd.aarch64.json"
    VULKAN_DRIVER="Turnip (Qualcomm Adreno Hardware Direct)"
elif [ -f /data/data/com.termux/files/usr/share/vulkan/icd.d/lvp_icd.aarch64.json ]; then
    export VK_ICD_FILENAMES="/data/data/com.termux/files/usr/share/vulkan/icd.d/lvp_icd.aarch64.json"
    VULKAN_DRIVER="Lavapipe (Mesa Vulkan 1.4)"
else
    VULKAN_DRIVER="System Default"
fi

# --- Step 3: Ensure compositor binary is up to date ---
echo "Checking compositor binary..."
ninja -C build &>/dev/null || true

# --- Step 4: Start compositor (High Refresh Rate) ---
TERMUXWC_FPS="${TERMUXWC_FPS:-120}"
echo "Starting termuxwc Sway-grade compositor (${TERMUXWC_FPS} FPS)..."
> "$LOG_FILE"
XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" ./build/termuxwc -H -r "$TERMUXWC_FPS" &> "$LOG_FILE" &
COMPOSITOR_PID=$!

sleep 0.5
if ! kill -0 "$COMPOSITOR_PID" &>/dev/null; then
    echo "ERROR: termuxwc failed to start. Logs:"
    cat "$LOG_FILE"
    exit 1
fi

# --- Step 5: Wait for Wayland socket ---
WAYLAND_SOCKET=""
for i in $(seq 1 30); do
    for sock in "$XDG_RUNTIME_DIR"/wayland-[0-9]*; do
        if [ -S "$sock" ]; then
            WAYLAND_SOCKET="$sock"
            break 2
        fi
    done
    sleep 0.1
done

if [ -z "$WAYLAND_SOCKET" ]; then
    echo "ERROR: Wayland socket was not created within 3 seconds."
    cat "$LOG_FILE"
    exit 1
fi

export WAYLAND_DISPLAY=$(basename "$WAYLAND_SOCKET")
echo "Wayland compositor ready on WAYLAND_DISPLAY=$WAYLAND_DISPLAY (socket: $WAYLAND_SOCKET)"

# --- Step 6: Start wayvnc (Uncapped / 120+ FPS with GPU Acceleration) ---
echo "Starting wayvnc VNC server (${TERMUXWC_FPS} FPS, GPU accelerated) on 0.0.0.0:5900..."
> "$VNC_LOG"
XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
    wayvnc -f "$TERMUXWC_FPS" -g 0.0.0.0:5900 &> "$VNC_LOG" &
VNC_PID=$!

sleep 0.5
if ! kill -0 "$VNC_PID" &>/dev/null; then
    echo "WARNING: wayvnc failed to start. Logs:"
    cat "$VNC_LOG"
else
    echo "wayvnc VNC server is ACTIVE on port 5900 (PID $VNC_PID)."
fi

# --- Step 7: Start terminal with GPU Acceleration (foot) ---
echo "Starting terminal (foot) with GPU acceleration..."
> "$FOOT_LOG"
XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
GALLIUM_DRIVER="$GALLIUM_DRIVER" \
MESA_GL_VERSION_OVERRIDE="$MESA_GL_VERSION_OVERRIDE" \
MESA_GLSL_VERSION_OVERRIDE="$MESA_GLSL_VERSION_OVERRIDE" \
VK_ICD_FILENAMES="$VK_ICD_FILENAMES" \
    foot &> "$FOOT_LOG" &
FOOT_PID=$!

sleep 0.5
if ! kill -0 "$FOOT_PID" &>/dev/null; then
    echo "WARNING: foot failed to start. Logs:"
    cat "$FOOT_LOG"
else
    echo "foot terminal is ACTIVE (PID $FOOT_PID)."
fi

echo ""
echo "=== Desktop Session is Ready ==="
echo "GPU Acceleration : $DETECTED_GPU ($ACCEL_TYPE)"
echo "OpenGL Backend   : ${GALLIUM_DRIVER:-none}"
echo "Vulkan Driver    : $VULKAN_DRIVER"
echo "Refresh Rate     : ${TERMUXWC_FPS} FPS (High-Refresh Streaming)"
echo "Connect VNC to   : 127.0.0.1:5900 (or localhost:5900)"
echo "Press Ctrl+C to stop the session."
echo ""

wait "$COMPOSITOR_PID"
