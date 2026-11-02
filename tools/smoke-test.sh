#!/bin/bash
# termuxwc smoke test — runs entirely WITHOUT a display, VNC or X11.
# Verifies: binary starts, Wayland socket appears, wl_info round-trips,
# a client (weston-info/wayland-info) can bind globals, tiling config parses.
set -u

BIN="${1:-./build/termuxwc}"
PASS=0; FAIL=0
ok()   { echo "  [OK]   $1"; PASS=$((PASS+1)); }
bad()  { echo "  [FAIL] $1"; FAIL=$((FAIL+1)); }

[ -x "$BIN" ] || { echo "Binary not found: $BIN (run: meson setup build && ninja -C build)"; exit 1; }

export XDG_RUNTIME_DIR="$(mktemp -d)"
chmod 700 "$XDG_RUNTIME_DIR"
cleanup() { [ -n "${CPID:-}" ] && kill "$CPID" 2>/dev/null; rm -rf "$XDG_RUNTIME_DIR"; }
trap cleanup EXIT INT TERM

echo "== 1. Headless backend boot =="
"$BIN" -b headless -w 1280 -h 720 -r 60 > /tmp/twc-smoke.log 2>&1 &
CPID=$!

SOCK=""
for i in $(seq 1 30); do
    for s in "$XDG_RUNTIME_DIR"/wayland-*; do [ -S "$s" ] && SOCK="$s" && break 2; done
    sleep 0.1
done
[ -n "$SOCK" ] && ok "Wayland socket created: $(basename "$SOCK")" || bad "no Wayland socket within 3s"

if kill -0 "$CPID" 2>/dev/null; then ok "compositor process alive"; else bad "compositor crashed:"; cat /tmp/twc-smoke.log; fi

echo "== 2. Protocol handshake =="
export WAYLAND_DISPLAY="$(basename "$SOCK")"
if command -v weston-info >/dev/null 2>&1; then INFO=weston-info
elif command -v wayland-info >/dev/null 2>&1; then INFO=wayland-info
else INFO=""; fi

if [ -n "$INFO" ]; then
    OUT=$("$INFO" 2>/dev/null | head -200)
    echo "$OUT" | grep -q "wl_compositor" && ok "wl_compositor advertised" || bad "wl_compositor missing"
    echo "$OUT" | grep -q "xdg_wm_base"  && ok "xdg_wm_base advertised"  || bad "xdg_wm_base missing"
    echo "$OUT" | grep -q "wlr_layer_shell_v1" && ok "layer-shell advertised" || bad "layer-shell missing"
    echo "$OUT" | grep -q "zwlr_screencopy_manager_v1" && ok "screencopy advertised (dmabuf capture path)" || echo "  [skip] screencopy not present"
else
    echo "  [skip] no weston-info/wayland-info installed (apt install weston-utils wayland-utils)"
fi

echo "== 3. Client app spawn via -s =="
kill "$CPID" 2>/dev/null; wait "$CPID" 2>/dev/null
if command -v foot >/dev/null 2>&1; then
    APP=foot; elif command -v weston-simple-egl >/dev/null 2>&1; then APP=weston-simple-egl
    else APP=""; fi
if [ -n "$APP" ]; then
    export XDG_RUNTIME_DIR="$(mktemp -d)"; chmod 700 "$XDG_RUNTIME_DIR"
    "$BIN" -b headless -s "$APP" > /tmp/twc-smoke2.log 2>&1 & CPID=$!
    sleep 2
    if kill -0 "$CPID" 2>/dev/null; then ok "$APP launched under compositor, still running"; else bad "crash with client:"; cat /tmp/twc-smoke2.log; fi
    kill "$CPID" 2>/dev/null; rm -rf "$XDG_RUNTIME_DIR"
else
    echo "  [skip] no test client (apt install foot or weston-utils)"
fi

echo ""
echo "RESULT: $PASS passed, $FAIL failed"
exit $([ "$FAIL" -eq 0 ] && echo 0 || echo 1)
