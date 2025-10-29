
/*
 * termuxwc: wlroots-0.20-dev tabanlı, TigerVNC ile çalışan
 * basit bir Termux Wayland compositor'ü.
 * Bu versiyon, Xvnc'yi tek başına X server olarak baslatir.
 */

#define _POSIX_C_SOURCE 200809L
#include <limits.h> // For PATH_MAX
#include <sys/stat.h> // For stat()
#include <sys/wait.h> // For waitpid(), WNOHANG
#include <errno.h>    // For errno
#include <unistd.h>    // For usleep()

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

#include <wayland-server-core.h>
#include <wayland-util.h> // wl_list için
#include <wlr/util/log.h>
#include <wlr/util/box.h> // wlr_box ve render işlemleri için

// wlroots backend ve render
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pass.h> // wlroots-0.20-dev'de wlr_render_pass için

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
// TigerVNC için gerekli protokoller (screencopy hala istemci için kullanılabilir)
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
// DÜZELTİLMİŞ RENDER DÖNGÜSÜ (wlroots-0.20-dev API - wlr_render_pass kullanarak)
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
        .when = {now.tv_sec}, // Use the time_t value (seconds since epoch) from the timespec, wrapped in braces
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


// Ana fonksiyon
int main(int argc, char *argv[]) {
    (void)argc; // Kullanılmayan parametre uyarısını sustur
    (void)argv;

    struct termuxwc_server server = {0};

    // Pencereleri tutacağımız listeyi başlat
    wl_list_init(&server.views);

    // pixman renderer'ı zorunlu kıl (Termux'ta OpenGL/Vulkan zor olabilir)
    //setenv("WLR_RENDERER", "pixman", 1);

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

    // Girdi (Seat) ve TigerVNC için sanal protokoller
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
    if (!wlr_screencopy_manager_v1_create(server.display)) {
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

    // --- PID Değişkenlerini Başlat ---
    pid_t vnc_pid = 0; // Artık sadece Xvnc PID'si gerekiyor
    pid_t xwayland_pid = 0;
    pid_t xterm_pid = 0;
    pid_t term_pid = 0;

    // --- START TigerVNC Server (Xvnc) as the sole X Server with Dynamic Display Number ---
    wlr_log(WLR_INFO, "Starting TigerVNC (Xvnc) as the sole X server with dynamic display...");
    int vnc_display_num = -1;
    char vnc_display_str[16] = {0}; // Initialize to empty string

    // Build the path for the Termux X11 directory
    char termux_x11_dir[PATH_MAX];
    const char *prefix = getenv("PREFIX");
    if (!prefix) {
        prefix = "/data/data/com.termux/files/usr"; // Default Termux prefix
    }
    snprintf(termux_x11_dir, sizeof(termux_x11_dir), "%s/tmp/.X11-unix", prefix);
    wlr_log(WLR_DEBUG, "Checking for free displays in: %s", termux_x11_dir);

    // Try to find a free display number (start from :10 to avoid common conflicts)
    for (int i = 10; i < 100; i++) {
        // Check if the lock file exists in the Termux-specific directory
        char lock_file[PATH_MAX];
        snprintf(lock_file, sizeof(lock_file), "%s/X%d-lock", termux_x11_dir, i);
        if (access(lock_file, F_OK) != 0) { // Lock file doesn't exist, this display might be free
            vnc_display_num = i;
            snprintf(vnc_display_str, sizeof(vnc_display_str), ":%d", i); // Set the display string
            wlr_log(WLR_DEBUG, "Found free display candidate: %s", vnc_display_str);
            break;
        }
    }

    if (vnc_display_num == -1) {
        wlr_log(WLR_ERROR, "No free Xvnc display found in %s after checking 90 possibilities.", termux_x11_dir);
        goto start_terminal; // Jump to launching terminal directly
    }

    // --- START TigerVNC Server (Xvnc) ---
    vnc_pid = fork();
    if (vnc_pid == 0) {
        // Child process: exec TigerVNC (Xvnc) as the X server itself
        // Set DISPLAY so X11 apps launched later connect to it
        setenv("DISPLAY", vnc_display_str, 1);
        // Command: Xvnc :<dynamic> -geometry 1280x720 -depth 24 -rfbport 5999 -localhost no -SecurityTypes None -desktop TermuxWC_Xvnc
        // NOTE: Using -SecurityTypes None is INSECURE. Configure a password in production.
        execl("/data/data/com.termux/files/usr/bin/Xvnc", "Xvnc",
              vnc_display_str,
              "-geometry", "1280x720", "-depth", "24",
              "-rfbport", "5999", "-localhost", "no", "-SecurityTypes", "None",
              "-desktop", "TermuxWC_Xvnc",
              (char *)NULL);
        perror("TigerVNC (Xvnc) exec failed");
        _exit(1);
    } else if (vnc_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to fork for TigerVNC (Xvnc)");
        wlr_log(WLR_ERROR, "TigerVNC launch failed.");
        goto start_terminal; // Jump to launching terminal directly
    }
    sleep(2); // Give Xvnc more time to initialize as the X server
    wlr_log(WLR_INFO, "TigerVNC (Xvnc) started on display %s (PID %d) on port 5999", vnc_display_str, vnc_pid);


    // --- Start xwayland ---
    wlr_log(WLR_INFO, "Starting xwayland...");
    xwayland_pid = fork();
    if (xwayland_pid == 0) {
        // Child process: exec xwayland
        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        execl("/data/data/com.termux/files/usr/bin/Xwayland", "Xwayland", "-rootless", "-listen", "tcp", "-core", (char *)NULL);
        perror("Xwayland exec failed");
        _exit(1);
    } else if (xwayland_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to fork for Xwayland");
        // Continue without Xwayland, only Wayland apps will work inside the compositor
    } else {
        wlr_log(WLR_INFO, "Xwayland started (PID %d) on its own display (e.g., :1000)", xwayland_pid);
    }
    sleep(1); // Give Xwayland a moment to start

    // --- Start an X11 application (e.g., xterm) on the Xvnc display ---
    wlr_log(WLR_INFO, "Starting xterm on Xvnc display %s...", vnc_display_str);
    xterm_pid = fork();
    if (xterm_pid == 0) {
        // Child process: exec xterm
        setenv("DISPLAY", vnc_display_str, 1); // Connect to the Xvnc display (which is now the X server)
        execl("/data/data/com.termux/files/usr/bin/xterm", "xterm", "-geometry", "80x24", "-bg", "black", "-fg", "white", (char *)NULL);
        perror("xterm exec failed");
        _exit(1);
    } else if (xterm_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to fork for xterm");
        // This is not fatal, VNC will still run, just without the xterm
    } else {
        wlr_log(WLR_INFO, "xterm started (PID %d) on Xvnc display %s", xterm_pid, vnc_display_str);
    }


start_terminal: // Label to jump to if Xvnc fails persistently
    // Terminali (foot) başlat
    term_pid = launch_pty_terminal(socket, xdg);
    if (term_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to launch terminal");
    } else {
        wlr_log(WLR_INFO, "Launched terminal (PID %d)", term_pid);
    }

    wlr_log(WLR_INFO, "TermuxWC running. VNC should be available on port 5999 via TigerVNC/Xvnc (acting as X server). Xwayland is running.");

    // Olay döngüsünü çalıştır
    wl_display_run(server.display);

    //
    // Cleanup (Hata ayıklama etiketleri)
    //
    // Kill child processes on exit if necessary
    if (vnc_pid > 0) { // This check is now safe due to initialization
        wlr_log(WLR_INFO, "Terminating TigerVNC (PID %d)...", vnc_pid);
        kill(vnc_pid, SIGTERM);
    }
    if (xwayland_pid > 0) { // This check is now safe due to initialization
        wlr_log(WLR_INFO, "Terminating Xwayland (PID %d)...", xwayland_pid);
        kill(xwayland_pid, SIGTERM);
    }
    if (xterm_pid > 0) { // This check is now safe due to initialization
        wlr_log(WLR_INFO, "Terminating xterm (PID %d)...", xterm_pid);
        kill(xterm_pid, SIGTERM);
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
