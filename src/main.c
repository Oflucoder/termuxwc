/*
 * termuxwc: wlroots-0.20-dev tabanlı, VNC çıkışı kullanan
 * basit bir Termux Wayland compositor'ü.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <termios.h>
#include <sys/stat.h>
#include <errno.h>
#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/util/log.h>

#include <rfb/rfb.h>

struct termuxwc_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_compositor *compositor;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_output_layout *output_layout;
    struct wlr_output *output;

    rfbScreenInfoPtr vnc;
    pthread_t vnc_thread;

    uint32_t width;
    uint32_t height;
    uint32_t *pixel_buffer;
};

static void *vnc_thread_fn(void *arg) {
    rfbScreenInfoPtr rfb = (rfbScreenInfoPtr)arg;
    rfbRunEventLoop(rfb, -1, FALSE);
    return NULL;
}

static int start_vnc_server(struct termuxwc_server *server, int width, int height) {
    rfbScreenInfoPtr rfb = rfbGetScreen(NULL, NULL, width, height, 8, 3, 4);
    if (!rfb) {
        wlr_log(WLR_ERROR, "rfbGetScreen failed");
        return -1;
    }

    rfb->frameBuffer = calloc((size_t)width * (size_t)height * 4, 1);
    if (!rfb->frameBuffer) {
        wlr_log(WLR_ERROR, "Failed to allocate VNC framebuffer");
        rfbScreenCleanup(rfb);
        return -1;
    }

    rfb->alwaysShared = TRUE;
    rfb->desktopName = strdup("termuxwc");
    rfb->serverFormat.redShift = 16;
    rfb->serverFormat.greenShift = 8;
    rfb->serverFormat.blueShift = 0;
    rfb->serverFormat.redMax = 255;
    rfb->serverFormat.greenMax = 255;
    rfb->serverFormat.blueMax = 255;
    rfb->bitsPerPixel = 32;
    rfb->depth = 24;
    rfb->port = 5900;

    rfbInitServer(rfb);
    server->vnc = rfb;

    if (pthread_create(&server->vnc_thread, NULL, vnc_thread_fn, rfb) != 0) {
        wlr_log(WLR_ERROR, "Failed to create VNC thread");
        rfbScreenCleanup(rfb);
        return -1;
    }

    wlr_log(WLR_INFO, "VNC server listening on port %d", rfb->port);
    return 0;
}

// Test pattern for VNC (since real rendering not possible in wlroots 0.20)
static void update_vnc_test_pattern(rfbScreenInfoPtr vnc) {
    if (!vnc || !vnc->frameBuffer) return;
    int w = vnc->width, h = vnc->height;
    uint32_t *fb = (uint32_t *)vnc->frameBuffer;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            fb[y * w + x] = (x % 256) << 16 | (y % 256) << 8 | 0xFF;
        }
    }
    rfbMarkRectAsModified(vnc, 0, 0, w, h);
}

static int vnc_timer_callback(void *data) {
    rfbScreenInfoPtr vnc = data;
    update_vnc_test_pattern(vnc);
    return 100; // ms
}

static pid_t launch_pty_terminal(const char *socket, const char *xdg_runtime) {
    struct termios termios;
    pid_t pid;
    int master;

    if (tcgetattr(STDIN_FILENO, &termios) == -1) {
        cfmakeraw(&termios);
    } else {
        cfmakeraw(&termios);
    }

    pid = forkpty(&master, NULL, &termios, NULL);
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_RUNTIME_DIR", xdg_runtime, 1);
        setenv("TERM", "xterm-256color", 1);
        setenv("COLORTERM", "truecolor", 1);

        execl("/data/data/com.termux/files/usr/bin/foot", "foot", (char *)NULL);
        execl("/data/data/com.termux/files/usr/bin/alacritty", "alacritty", (char *)NULL);
        execl("/data/data/com.termux/files/usr/bin/weston-terminal", "weston-terminal", (char *)NULL);

        perror("Failed to launch any terminal");
        _exit(1);
    }
    return pid;
}

int main(int argc, char *argv[]) {
    struct termuxwc_server server = {0};

    setenv("WLR_RENDERER", "pixman", 1);

    server.display = wl_display_create();
    if (!server.display) {
        wlr_log(WLR_ERROR, "Failed to create Wayland display");
        return 1;
    }
    server.event_loop = wl_display_get_event_loop(server.display);

    server.width = 1280;
    server.height = 720;

    server.pixel_buffer = calloc((size_t)server.width * server.height, 4);
    if (!server.pixel_buffer) {
        wlr_log(WLR_ERROR, "Failed to allocate pixel_buffer");
        goto fail_display;
    }

    if (start_vnc_server(&server, server.width, server.height) < 0) {
        goto fail_pixel_buffer;
    }

    // Create backend
    server.backend = wlr_headless_backend_create(server.event_loop);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "Failed to create headless backend");
        goto fail_vnc;
    }

    // Create renderer
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "Failed to create renderer");
        goto fail_backend;
    }

    // Create compositor
    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    if (!server.compositor) {
        wlr_log(WLR_ERROR, "Failed to create compositor");
        goto fail_renderer;
    }

    // Create SHM with fallback formats (critical for terminals)
    const uint32_t fallback_formats[] = { 0x34325258, 0x34325241 }; // XRGB, ARGB
    if (!wlr_shm_create(server.display, 1, fallback_formats, 2)) {
        wlr_log(WLR_ERROR, "SHM creation failed — terminals may not work");
    } else {
        wlr_log(WLR_INFO, "wl_shm created with fallback formats");
    }

    // Create xdg-shell
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    if (!server.xdg_shell) {
        wlr_log(WLR_ERROR, "Failed to create xdg-shell");
        goto fail_compositor;
    }

    // Create output layout
    server.output_layout = wlr_output_layout_create(server.display);
    if (!server.output_layout) {
        wlr_log(WLR_ERROR, "Failed to create output layout");
        goto fail_xdg;
    }

    // Start backend
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        goto fail_layout;
    }

    // ➤ Create headless output (wlroots 0.20 API)
    server.output = wlr_headless_add_output(server.backend, server.width, server.height);
    if (!server.output) {
        wlr_log(WLR_ERROR, "Failed to create headless output");
        goto fail_layout;
    }
    server.output->name = "VNC-1";
    wlr_output_layout_add_auto(server.output_layout, server.output);

    // Wayland socket
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to create Wayland socket");
        goto fail_layout;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket);

    // XDG_RUNTIME_DIR
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg) {
        xdg = "/data/data/com.termux/files/usr/tmp/wayland";
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        mkdir(xdg, 0700);
    }

    // Launch terminal
    pid_t term_pid = launch_pty_terminal(socket, xdg);
    if (term_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to launch terminal");
    } else {
        wlr_log(WLR_INFO, "Launched terminal (PID %d)", term_pid);
    }

    // Start VNC update timer
    wl_event_loop_add_timer(server.event_loop, vnc_timer_callback, server.vnc);

    wlr_log(WLR_INFO, "TermuxWC running. Connect via VNC to port 5900.");
    wl_display_run(server.display);

    // Cleanup
fail_layout:
    if (server.output_layout) wlr_output_layout_destroy(server.output_layout);
fail_xdg:
    // No destroy function for xdg_shell
fail_compositor:
    // No destroy function for compositor
fail_renderer:
    if (server.renderer) wlr_renderer_destroy(server.renderer);
fail_backend:
    if (server.backend) wlr_backend_destroy(server.backend);
fail_vnc:
    if (server.vnc) {
        free(server.vnc->frameBuffer);
        rfbScreenCleanup(server.vnc);
    }
fail_pixel_buffer:
    free(server.pixel_buffer);
fail_display:
    if (server.display) wl_display_destroy(server.display);
    return 1;
}
