#define _POSIX_C_SOURCE 200809L

#include <stdlib.h>
#include <wlr/types/wlr_data_device.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <wlr/types/wlr_subcompositor.h>
#include <sys/stat.h>
#include <wayland-server-core.h>
#include <wlr/backend/headless.h>
#include <wlr/backend.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/util/log.h>
#include <rfb/rfb.h>
#include <wlr/types/wlr_shm.h>

struct termuxwc_server {
    struct wl_display *display;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_compositor *compositor;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_output_layout *output_layout;
    struct wlr_output *output;
};

static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct wlr_xdg_surface *xdg_surface = data;
    wlr_log(WLR_INFO, "New xdg surface");
    if (xdg_surface->toplevel) {
        wlr_log(WLR_INFO, "  app_id: %s", xdg_surface->toplevel->app_id ?: "(none)");
        wlr_log(WLR_INFO, "  title: %s", xdg_surface->toplevel->title ?: "(none)");
    }
}

static void *vnc_thread_func(void *arg) {
    rfbScreenInfoPtr screen = (rfbScreenInfoPtr)arg;
    rfbRunEventLoop(screen, -1, TRUE);
    return NULL;
}

int main(int argc, char *argv[]) {
    wlr_log_init(WLR_INFO, NULL);

    const char *xdg_runtime_dir = getenv("XDG_RUNTIME_DIR");
    if (!xdg_runtime_dir) {
        xdg_runtime_dir = "/data/data/com.termux/files/usr/tmp/wayland";
        setenv("XDG_RUNTIME_DIR", xdg_runtime_dir, 1);
        mkdir(xdg_runtime_dir, 0700);
    }

    struct termuxwc_server server = {0};
    server.display = wl_display_create();
    if (!server.display) {
        fprintf(stderr, "Failed to create wl_display\n");
        return EXIT_FAILURE;
    }

    server.backend = wlr_headless_backend_create(wl_display_get_event_loop(server.display));
    if (!server.backend) {
        wlr_log(WLR_ERROR, "Failed to create headless backend");
        goto fail_display;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "Failed to create renderer");
        goto fail_display;
    }

    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    if (!server.compositor) {
        wlr_log(WLR_ERROR, "Failed to create compositor");
        goto fail_display;
    }

    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    if (!server.xdg_shell) {
        wlr_log(WLR_ERROR, "Failed to create xdg-shell");
        goto fail_display;
    }

    struct wl_listener new_xdg_surface = {0};
    new_xdg_surface.notify = handle_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &new_xdg_surface);

    server.output_layout = wlr_output_layout_create(server.display);
    if (!server.output_layout) {
        wlr_log(WLR_ERROR, "Failed to create output layout");
        goto fail_display;
    }

    /* ---------------- VNC ---------------- */
    rfbScreenInfoPtr vnc_screen = rfbGetScreen(&argc, argv, 800, 600, 8, 3, 4);
    if (!vnc_screen) {
        fprintf(stderr, "rfbGetScreen failed\n");
        goto fail_display;
    }

    size_t fbsize = 800 * 600 * 4;
    vnc_screen->frameBuffer = calloc(1, fbsize);
    if (!vnc_screen->frameBuffer) {
        fprintf(stderr, "VNC framebuffer alloc failed\n");
        rfbScreenCleanup(vnc_screen);
        goto fail_display;
    }

    vnc_screen->serverFormat.redShift = 16;
    vnc_screen->serverFormat.greenShift = 8;
    vnc_screen->serverFormat.blueShift = 0;
    vnc_screen->serverFormat.redMax = 255;
    vnc_screen->serverFormat.greenMax = 255;
    vnc_screen->serverFormat.blueMax = 255;
    vnc_screen->bitsPerPixel = 32;
    vnc_screen->depth = 24;
    vnc_screen->alwaysShared = TRUE;
    vnc_screen->port = 5901;

    rfbInitServer(vnc_screen);
    fprintf(stderr, "VNC server listening on port %d\n", vnc_screen->port);

    pthread_t vnc_thread;
    if (pthread_create(&vnc_thread, NULL, vnc_thread_func, vnc_screen) != 0) {
        fprintf(stderr, "Failed to start VNC thread\n");
        free(vnc_screen->frameBuffer);
        rfbScreenCleanup(vnc_screen);
        goto fail_display;
    }
    pthread_detach(vnc_thread);

    /* Add Wayland socket */
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to add socket");
        goto fail_vnc;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket);

    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        goto fail_vnc;
    }

    /* ➤ Critical: create output BEFORE SHM */
    server.output = wlr_headless_add_output(server.backend, 800, 600);
    if (!server.output) {
        wlr_log(WLR_ERROR, "Failed to create headless output");
        goto fail_vnc;
    }
    server.output->name = "VNC";
    wlr_output_layout_add_auto(server.output_layout, server.output);

    /* ➤ Now create SHM (should work if wlroots is patched) */
    struct wlr_shm *shm = wlr_shm_create(server.display, 1, NULL, 0);
    if (!shm) {
        wlr_log(WLR_ERROR, "SHM not available — clients may fail");
        // Continue anyway for testing
    }

    /* ➤ Launch terminal BEFORE main loop */
    pid_t pid = fork();
    if (pid == 0) {
        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_RUNTIME_DIR", xdg_runtime_dir, 1);
        execl("/data/data/com.termux/files/usr/bin/alacritty",
              "alacritty", (char *)NULL);
        perror("Failed to launch alacritty");
        _exit(1);
    } else if (pid < 0) {
        wlr_log(WLR_ERROR, "fork failed");
    }

    wlr_log(WLR_INFO, "TermuxWC running. Connect via VNC to port 5901.");
    wl_display_run(server.display);

    /* Cleanup */
fail_vnc:
    if (vnc_screen) {
        free(vnc_screen->frameBuffer);
        rfbScreenCleanup(vnc_screen);
    }
fail_display:
    wl_display_destroy(server.display);
    return EXIT_SUCCESS;
}
