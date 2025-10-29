
/*
 * termuxwc: wlroots-0.20-dev tabanlı, wayvnc ile çalışan
 * basit bir Termux Wayland compositor'ü.
 */

#define _POSIX_C_SOURCE 200809L

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <pty.h>
#include <termios.h>
#include <sys/stat.h>
#include <errno.h>

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

// wayvnc için gerekli protokoller
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>


struct termuxwc_server;

// Her bir pencereyi (foot) temsil eden yapı
struct termuxwc_view {
    struct wl_list link; // Pencereleri bir listede tutmak için
    struct termuxwc_server *server;
    struct wlr_xdg_surface *xdg_surface;

    struct wl_listener destroy;
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
        // DÜZELTME 1: xg_runtime -> xdg_runtime
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

//
// DÜZELTİLMİŞ RENDER DÖNGÜSÜ (wlroots-0.20-dev API)
//
// Her karede (frame) çağrılacak olan render fonksiyonumuz
static void handle_output_frame(struct wl_listener *listener, void *data) {
    struct termuxwc_server *server = wl_container_of(listener, server, output_frame);
    struct wlr_output *output = server->output;

    // Fonksiyon 3 argüman alıyor
    struct wlr_render_pass *pass = wlr_output_begin_render_pass(output, NULL, NULL);
    if (pass == NULL) {
        return;
    }

    // Ekranı temizlemek için tam ekran bir dikdörtgen çiziyoruz.
    float color[] = {0.3f, 0.3f, 0.3f, 1.0f}; // Koyu gri arka plan
    struct wlr_box full_output_box = {
        .x = 0,
        .y = 0,
        .width = output->width,
        .height = output->height,
    };
    struct wlr_render_rect_options rect_options = {
        // DÜZELTME 2: .box bir pointer değil, struct'ın kendisi
        .box = full_output_box,
        .color = {color[0], color[1], color[2], color[3]},
    };
    wlr_render_pass_add_rect(pass, &rect_options);


    // .alpha alanı const float* bekliyor
    static const float alpha = 1.0f; // .alpha için bir pointer sağlamak amacıyla

    // Takip ettiğimiz her "view" (pencere) için render yap
    struct termuxwc_view *view;
    wl_list_for_each(view, &server->views, link) {
        struct wlr_surface *surface = view->xdg_surface->surface;
        
        // Yüzeyin dokusunu (texture) al
        struct wlr_texture *texture = wlr_surface_get_texture(surface);
        if (texture == NULL) {
            continue;
        }

        // Yüzeyi (texture) render pass'a ekle
        struct wlr_render_texture_options tex_options = {
            .texture = texture,
            .dst_box = {
                .x = 0, // Şimdilik 0,0'a çizelim
                .y = 0,
                .width = surface->current.width,
                .height = surface->current.height
            },
            .alpha = &alpha,
        };

        wlr_render_pass_add_texture(pass, &tex_options);
        
        // Çizim tamamlandı, yüzeye haber ver
        wlr_surface_send_frame_done(surface, (struct timespec*)data);
    }

    // Render işlemini bitir ve gönder
    if (!wlr_render_pass_submit(pass)) {
        wlr_log(WLR_ERROR, "Failed to submit render pass");
    }
}

// Bir pencere (view) kapandığında çağrılır
static void handle_view_destroy(struct wl_listener *listener, void *data) {
    (void)data; // Kullanılmayan parametre uyarısını sustur
    struct termuxwc_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->link);
    free(view);
}

// Yeni bir pencere (foot) açıldığında çağrılır
static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct termuxwc_server *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    // Yeni bir "view" oluştur ve listeye ekle
    struct termuxwc_view *view = calloc(1, sizeof(struct termuxwc_view));
    view->server = server;
    view->xdg_surface = xdg_surface;
    wl_list_insert(&server->views, &view->link);

    // Bu pencere kapandığında haberdar olmak için 'destroy' sinyalini dinle
    view->destroy.notify = handle_view_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
    wlr_xdg_surface_schedule_configure(xdg_surface);
}


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

    // Girdi (Seat) ve wayvnc için sanal protokoller
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

    // Terminali (foot) başlat
    pid_t term_pid = launch_pty_terminal(socket, xdg);
    if (term_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to launch terminal");
    } else {
        wlr_log(WLR_INFO, "Launched terminal (PID %d)", term_pid);
    }

    // wayvnc'yi ayrı bir process'te başlat
    if (fork() == 0) {
        setenv("WAYLAND_DISPLAY", socket, 1);
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        execl("/data/data/com.termux/files/usr/bin/wayvnc", "wayvnc", "0.0.0.0", "5999", (char *)NULL);
        perror("wayvnc launch failed");
        _exit(1);
    }

    wlr_log(WLR_INFO, "TermuxWC running. Connect via VNC to port 5999.");
    
    // Olay döngüsünü çalıştır
    wl_display_run(server.display);
    
    //
    // Cleanup (Hata ayıklama etiketleri)
    //
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
