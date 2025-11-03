/*
 * termuxwc: wlroots-0.19.1 tabanlı, neatvnc ile çalışan
 * basit bir Termux Wayland compositor'ü.
 * BU SÜRÜM: Saf Wayland. Xvfb/Xvnc/xterm KULLANMAZ.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pty.h>
#include <termios.h>
#include <sys/stat.h>
#include <sys/types.h> // access() için
#include <errno.h>
#include <signal.h> // For kill()
#include <sys/wait.h> // For waitpid()

#include <wayland-server-core.h>
#include <wayland-util.h> // wl_list için
#include <wlr/util/log.h>
#include <wlr/util/box.h> // wlr_box ve render işlemleri için

// wlroots backend ve render
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pass.h> // wlroots-0.19.1'de wlr_render_pass için

// wlroots protokolleri
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_surface.h>
// neatvnc için gerekli protokoller (doğrudan screencopy)
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/matrix.h> // Potentially needed if using matrix functions later, though not directly used in the new pass API for surfaces

struct termuxwc_server;

// Her bir pencereyi (foot) temsil eden yapı
struct termuxwc_view {
    struct wl_list link; // Pencereleri bir listede tutmak için
    struct termuxwc_server *server;
    struct wlr_xdg_surface *xdg_surface;

    // Sinyal dinleyicileri
    struct wl_listener destroy;
    struct wl_listener commit; // 'map' yerine 'commit' dinleyeceğiz
};

// Ana compositor sunucu yapısı
struct termuxwc_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;

    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_compositor *compositor;
    struct wlr_output_layout *output_layout;
    struct wlr_output *output; // Headless VNC çıktımız

    struct wlr_seat *seat;
    struct wlr_xdg_shell *xdg_shell;

    struct wl_list views; // Takip ettiğimiz tüm pencerelerin listesi

    // Sinyal dinleyicileri
    struct wl_listener new_xdg_surface;
    struct wl_listener output_frame;
};

// VNC'ye bağlanan istemci için terminal (foot) başlatan fonksiyon
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
        // Çocuk process
        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_RUNTIME_DIR", xdg_runtime, 1); // 'xg_runtime' hatası düzeltildi
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

//
// DÜZELTİLMİŞ RENDER DÖNGÜSÜ (wlroots-0.19.1 API - wlr_render_pass kullanarak)
//
static void handle_output_frame(struct wl_listener *listener, void *data) {
    (void)data; // Suppress unused warning
    struct termuxwc_server *server = wl_container_of(listener, server, output_frame);
    struct wlr_output *output = server->output;

    // --- Get the time *before* starting the render pass for potential use in send_present ---
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // Begin a new render pass for the output
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(output, NULL, NULL);
    if (pass == NULL) {
        wlr_log(WLR_ERROR, "Failed to begin render pass");
        wlr_output_schedule_frame(output);
        return;
    }

    // Add background rectangle
    float bg_color[] = {0.3f, 0.3f, 0.3f, 1.0f};
    struct wlr_box bg_box = { .x = 0, .y = 0, .width = output->width, .height = output->height };
    wlr_render_pass_add_rect(pass, &(struct wlr_render_rect_options){
        .box = bg_box, // Pass struct value directly
        .color = {bg_color[0], bg_color[1], bg_color[2], bg_color[3]},
    });

    // Render all mapped surfaces using the render pass
    struct termuxwc_view *view;
    wl_list_for_each(view, &server->views, link) {
        struct wlr_xdg_surface *xdg_surface = view->xdg_surface;
        if (!xdg_surface->surface->mapped) continue;

        struct wlr_surface *surface = xdg_surface->surface;
        struct wlr_texture *texture = wlr_surface_get_texture(surface);
        if (!texture) continue; // Skip surfaces without a texture

        // Define the destination box (position and size on screen)
        // Use current geometry for position and size
        struct wlr_box dst_box = xdg_surface->current.geometry;

        // Define source box (region of the texture to use, NULL means full texture)
        // In this version, the struct field is a value, so initialize an empty one.
        // Passing an empty struct implies the full texture.
        struct wlr_fbox src_box = {0}; // Use entire texture

        // Define alpha (1.0f for opaque)
        float alpha = 1.0f;

        // Add the texture to the render pass
        wlr_render_pass_add_texture(pass, &(struct wlr_render_texture_options){
            .texture = texture,
            .src_box = src_box,    // Pass struct value directly, not a pointer
            .dst_box = dst_box,    // Pass struct value directly, not a pointer
            .alpha = &alpha,       // Alpha is still a pointer
            .transform = WL_OUTPUT_TRANSFORM_NORMAL, // Adjust if needed for rotation/flipping
            .filter_mode = WLR_SCALE_FILTER_BILINEAR, // Filtering mode
            .blend_mode = WLR_RENDER_BLEND_MODE_PREMULTIPLIED, // Default blend mode
        });

        // Send frame done to the client so it renders the next frame
        // Use the 'now' time captured *before* the render pass started
        wlr_surface_send_frame_done(surface, &now);
    } // End of wl_list_for_each loop

    // Submit the combined render pass (background + surfaces)
    bool submit_ok = wlr_render_pass_submit(pass);
    if (!submit_ok) {
        wlr_log(WLR_ERROR, "Failed to submit render pass");
        // Even if submit failed, we should still schedule the next frame.
        // However, sending present after a *failed* submit might be incorrect or meaningless.
        // It's safer not to send present if the pass failed.
        wlr_output_schedule_frame(output);
        return;
    }

    // 🔑 CRITICAL: Tell wlroots the frame was presented for screencopy
    // Populate the present event structure. Using the time captured before the pass is common.
    struct wlr_output_event_present present_event = {
        .when = { .tv_sec = now.tv_sec, .tv_nsec = now.tv_nsec }, // Use the full timespec, wrapped in braces { }
        .seq = 0, // Often set by backend or optional
        .flags = WLR_OUTPUT_PRESENT_VSYNC, // Indicate VSync (or similar timing event), common default
        // Omit other potentially non-existent or backend-specific fields
    };
    // Note: Depending on the backend and version, other fields might be needed or automatically filled.
    // The 'when' field (or its equivalent value) is often key.

    wlr_output_send_present(output, &present_event);
    wlr_log(WLR_DEBUG, "Sent present event for screencopy.");

    // Schedule the next frame to keep the compositor running
    wlr_output_schedule_frame(output);
}


static void handle_view_commit(struct wl_listener *listener, void *data) {
    (void)data;
    struct termuxwc_view *view = wl_container_of(listener, view, commit);

    if (!view->xdg_surface->configured) {
        wlr_xdg_surface_schedule_configure(view->xdg_surface);
    }

    // Optional log inside function
    if (view->xdg_surface->surface->mapped) {
        wlr_log(WLR_INFO, "Surface mapped! Will be rendered.");
    }

    wlr_log(WLR_DEBUG, "Commit received for surface %p", (void*)view->xdg_surface->surface);
}// Kapanış parantezi eklendi



// Bir pencere (view) kapandığında çağrılır
static void handle_view_destroy(struct wl_listener *listener, void *data) {
    (void)data; // Kullanılmayan parametre uyarısını sustur
    struct termuxwc_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->link);
    free(view);
}

//
// DÜZELTME: 'configure' işlemini 'map' yerine 'commit' sinyalinde yap
//
// Yüzey (pencere) 'commit' yaptığında (içeriğini güncellediğinde) çağrılır
// Kapanış parantezi eklendi


// Yeni bir pencere (foot) açıldığında çağrılır
static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct termuxwc_server *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    // Yeni bir "view" oluştur ve listeye ekle
    struct termuxwc_view *view = calloc(1, sizeof(struct termuxwc_view));
    view->server = server;
    view->xdg_surface = xdg_surface;
    wl_list_insert(&server->views, &view->link);

    // 'destroy' sinyalini dinle
    view->destroy.notify = handle_view_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &view->destroy);

    // DÜZELTME: 'map' yerine 'commit' sinyalini dinle
    view->commit.notify = handle_view_commit;
    wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);
    wlr_log(WLR_INFO, "New XDG surface created: %p", (void*)xdg_surface);
}
// --- START neatvnc (using the built png-server example) ---
static pid_t start_neatvnc(const char *socket, const char *xdg_runtime_dir) {
    wlr_log(WLR_INFO, "Starting neatvnc (png-server example)...");
    pid_t neatvnc_pid = fork();
    if (neatvnc_pid == 0) {
        // Child process: exec the built neatvnc example server (png-server)
        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_RUNTIME_DIR", xdg_runtime_dir, 1);
        // Command: ./build/examples/png-server -o :0 -p 5999 -l 0.0.0.0
        // NOTE: Using -SecurityTypes None is INSECURE. Configure a password in production.
        // NOTE: The arguments might differ slightly for png-server. Check its source or help.
        // Common arguments for neatvnc examples: -o (output/display), -p (port), -l (listen address)
        execl("./neatvnc/build/examples/png-server", "png-server",
              "./wallpaper.jpeg"
              "-o", ":0", // Capture output 0 of the compositor (might need adjustment)
              "-p", "5999", // VNC port
              "-l", "0.0.0.0", // Listen on all interfaces (INSECURE, use 127.0.0.1 in production)
              
              // "-SecurityTypes", "None", // Check if png-server supports this or uses a different method
              (char *)NULL);
        perror("neatvnc (png-server) exec failed");
        _exit(1);
    } else if (neatvnc_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to fork for neatvnc (png-server)");
        return -1;
    } else {
        wlr_log(WLR_INFO, "neatvnc (png-server) started (PID %d) on port 5999", neatvnc_pid);
        return neatvnc_pid;
    }
}
// --- END neatvnc ---



// Ana fonksiyon
int main(int argc, char *argv[]) {
    (void)argc; // Kullanılmayan parametre uyarısını sustur
    (void)argv;

    struct termuxwc_server server = {0};

    // Pencereleri tutacağımız listeyi başlat
    wl_list_init(&server.views);

    // pixman renderer'ı zorunlu kıl (Termux'ta OpenGL/Vulkan zor olabilir)
    setenv("WLR_RENDERER", "pixman", 1);

    server.display = wl_display_create();
    if (!server.display) {
        wlr_log(WLR_ERROR, "Failed to create Wayland display");
        return 1;
    }
    server.event_loop = wl_display_get_event_loop(server.display);

    const int width = 1280;
    const int height = 720;

    // Headless backend (görüntüyü fiziksel bir ekrana değil, hafızaya çizer)
    server.backend = wlr_headless_backend_create(server.event_loop);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "Failed to create headless backend");
        goto fail_display;
    }

    // Renderer (çizim motoru)
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "Failed to create renderer");
        goto fail_backend;
    }

    // Temel compositor bileşenleri
    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    if (!server.compositor) {
        wlr_log(WLR_ERROR, "Failed to create compositor");
        goto fail_renderer;
    }
    wlr_subcompositor_create(server.display);
    wlr_data_device_manager_create(server.display);

    // Girdi (Seat) ve neatvnc için sanal protokoller
    server.seat = wlr_seat_create(server.display, "seat0");
    if (!server.seat) {
        wlr_log(WLR_ERROR, "Failed to create seat");
        goto fail_compositor;
    }
    if (!wlr_virtual_keyboard_manager_v1_create(server.display)) {
        wlr_log(WLR_ERROR, "Failed to create virtual keyboard manager");
        goto fail_seat;
    }
    if (!wlr_virtual_pointer_manager_v1_create(server.display)) {
        wlr_log(WLR_ERROR, "Failed to create virtual pointer manager");
        goto fail_seat;
    }
    if (!wlr_screencopy_manager_v1_create(server.display)) { // CRITICAL for neatvnc
        wlr_log(WLR_ERROR, "Failed to create screencopy manager");
        goto fail_seat;
    }

    // SHM (Paylaşımlı Hafıza)
    const uint32_t fallback_formats[] = { 0x34325258, 0x34325241 }; // XRGB, ARGB
    if (!wlr_shm_create(server.display, 1, fallback_formats, 2)) {
        wlr_log(WLR_ERROR, "SHM creation failed");
    }

    // XDG-Shell (pencereleri yönetmek için)
    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    if (!server.xdg_shell) {
        wlr_log(WLR_ERROR, "Failed to create xdg-shell");
        goto fail_compositor;
    }
    // Yeni pencere sinyalini dinle
    server.new_xdg_surface.notify = handle_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);

    // Çıktı layout'u
    server.output_layout = wlr_output_layout_create(server.display);
    if (!server.output_layout) {
        wlr_log(WLR_ERROR, "Failed to create output layout");
        goto fail_xdg;
    }

    // Backend'i başlat
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        goto fail_layout;
    }

    // Sanal VNC çıktısını oluştur
    server.output = wlr_headless_add_output(server.backend, width, height);
    if (!server.output) {
        wlr_log(WLR_ERROR, "Failed to create headless output");
        goto fail_layout;
    }
    server.output->name = "VNC-1";

    // Render döngüsü için 'frame' sinyalini dinle
    server.output_frame.notify = handle_output_frame;
    wl_signal_add(&server.output->events.frame, &server.output_frame);

    wlr_output_layout_add_auto(server.output_layout, server.output);

    // Başlangıçta bir kare çiz
    wlr_output_schedule_frame(server.output);

    // Wayland soketini oluştur
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to create Wayland socket");
        goto fail_layout;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket);

    // XDG_RUNTIME_DIR (Termux için ayarla)
    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg) {
        xdg = "/data/data/com.termux/files/usr/tmp/wayland";
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        mkdir(xdg, 0700);
    }

    // --- START neatvnc ---
     pid_t neatvnc_pid = start_neatvnc(socket, xdg);
    if (neatvnc_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to start neatvnc (png-server)");
        // This is not fatal, the compositor will still run, just no VNC access
    } else {
        wlr_log(WLR_INFO, "Launched neatvnc (png-server) (PID %d)", neatvnc_pid);
    }
    // --- START neatvnc (using the built png-server example) ---
    
 // --- END neatvnc --- 
    //term 
      // --- START TERMINAL ---
    // Launch a Wayland terminal (e.g., foot) inside the compositor
    pid_t term_pid = launch_pty_terminal(socket, xdg); // Use the socket and xdg_runtime_dir from main
    if (term_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to launch terminal");
    } else {
        wlr_log(WLR_INFO, "Launched terminal (PID %d)", term_pid);
    }
    // --- END START TERMINAL ---
    // Olay döngüsünü çalıştır
    wl_display_run(server.display);

    //
    // Cleanup (Hata ayıklama etiketleri)
    //
    // Kill child processes on exit if necessary
    if (neatvnc_pid > 0) {
        wlr_log(WLR_INFO, "Terminating neatvnc (PID %d)...", neatvnc_pid);
        kill(neatvnc_pid, SIGTERM);
        int status;
        waitpid(neatvnc_pid, &status, 0);
        wlr_log(WLR_INFO, "neatvnc (PID %d) terminated.", neatvnc_pid);
    }
    
    
    if (term_pid > 0) {
        wlr_log(WLR_INFO, "Terminating terminal (PID %d)...", term_pid);
        kill(term_pid, SIGTERM);
        int status;
        waitpid(term_pid, &status, 0);
        wlr_log(WLR_INFO, "Terminal (PID %d) terminated.", term_pid);
    }
    
fail_seat:
    if (server.seat) wlr_seat_destroy(server.seat);
fail_layout:
    if (server.output_layout) wlr_output_layout_destroy(server.output_layout);
fail_xdg:
fail_compositor:
fail_renderer:
    if (server.renderer) wlr_renderer_destroy(server.renderer);
fail_backend:
    if (server.backend) wlr_backend_destroy(server.backend);
fail_display:
    if (server.display) wl_display_destroy(server.display);
    return 1;
}
