#!/bin/bash

# --- termuxwc Startup Manager ---
echo "=== termuxwc Desktop Session ==="

export XDG_RUNTIME_DIR="/data/data/com.termux/files/usr/tmp/xdg-runtime"
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"

# Clean up any stale processes or lockfiles from previous runs
pkill -9 -f termuxwc &>/dev/null || true
pkill -9 -f wayvnc &>/dev/null || true
pkill -9 -f foot &>/dev/null || true
rm -f "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/wayvnc* &>/dev/null || true

LOG_FILE="termuxwc.log"
VNC_LOG="wayvnc.log"
FOOT_LOG="foot.log"

COMPOSITOR_PID=
VNC_PID=
FOOT_PID=

cleanup() {
    echo ""
    echo "=== Shutting down termuxwc session ==="
    [ -n "$FOOT_PID" ] && kill "$FOOT_PID" &>/dev/null
    [ -n "$VNC_PID" ] && kill "$VNC_PID" &>/dev/null
    [ -n "$COMPOSITOR_PID" ] && kill "$COMPOSITOR_PID" &>/dev/null
    rm -f "$XDG_RUNTIME_DIR"/wayland-* "$XDG_RUNTIME_DIR"/wayvnc* &>/dev/null || true
    echo "All processes stopped."
    exit 0
}
trap cleanup INT TERM EXIT

# Step 1: Ensure compositor binary is up to date
echo "Checking compositor binary..."
ninja -C build &>/dev/null || true

# Step 2: Start compositor
echo "Starting termuxwc..."
> "$LOG_FILE"
XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" ./build/termuxwc &> "$LOG_FILE" &
COMPOSITOR_PID=$!

sleep 0.5
if ! kill -0 "$COMPOSITOR_PID" &>/dev/null; then
    echo "ERROR: termuxwc failed to start. Logs:"
    cat "$LOG_FILE"
    exit 1
fi

# Step 3: Wait for Wayland socket
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

# Step 4: Start wayvnc
echo "Starting wayvnc VNC server on 0.0.0.0:5900..."
> "$VNC_LOG"
XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
    wayvnc 0.0.0.0:5900 &> "$VNC_LOG" &
VNC_PID=$!

sleep 0.5
if ! kill -0 "$VNC_PID" &>/dev/null; then
    echo "WARNING: wayvnc failed to start. Logs:"
    cat "$VNC_LOG"
else
    echo "wayvnc VNC server is ACTIVE on port 5900 (PID $VNC_PID)."
fi

# Step 5: Start terminal (foot)
echo "Starting terminal (foot)..."
> "$FOOT_LOG"
XDG_RUNTIME_DIR="$XDG_RUNTIME_DIR" \
WAYLAND_DISPLAY="$WAYLAND_DISPLAY" \
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
echo "Connect your VNC client to: 127.0.0.1:5900 (or localhost:5900)"
echo "Press Ctrl+C to stop the session."
echo ""

wait "$COMPOSITOR_PID"
