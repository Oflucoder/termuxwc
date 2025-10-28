/*
 * termuxwc: wlroots-0.20-dev tabanlı, VNC çıkışı kullanan
 * basit bir Termux Wayland compositor'ü.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <pty.h>
#include <pixman.h>
#include <termios.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h> // clock_gettime için
#include <wayland-server.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/pixman.h>
#include <wlr/util/log.h>
#include <wlr/util/matrix.h>      // GEREKLİ: Yeni Matrix API'si (wlr_matrix_project_box)
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_surface.h> // GEREKLİ: wlr_surface ve render için
#include <wlr/types/wlr_buffer.h> // GEREKLİ: Yeni Buffer API'si (Render V7)
#include <rfb/rfb.h>   // libvncserver
#include <sys/stat.h>  // for mkdir
#include <errno.h>

// İleri bildirimler
struct termuxwc_server;
static void handle_new_xdg_surface(struct wl_listener *listener, void *data);
static void handle_output_frame(struct wl_listener *listener, void *data);

// EKLENDİ: XDG pencere (view) yönetimi için struct
struct termuxwc_view {
    struct wl_list link;
    struct termuxwc_server *server;
    struct wlr_xdg_surface *xdg_surface;

    struct wl_listener map;
    struct wl_listener unmap;
    struct wl_listener destroy;
    struct wl_listener commit;
    
    // EKLENDİ: Konumu saklamak için (wlroots 0.20+ değişikliği)
    double x, y;
};

struct termuxwc_server {
    struct wl_display *display;
    struct wl_event_loop *event_loop;
    struct wlr_backend *backend;
    struct wlr_renderer *renderer;
    struct wlr_compositor *compositor;
    struct wlr_xdg_shell *xdg_shell;
    struct wlr_output_layout *output_layout;

    rfbScreenInfoPtr vnc;
    pthread_t vnc_thread;

    uint32_t width;
    uint32_t height;
    uint32_t *pixel_buffer;  // VNC'ye kopyalama için ara bellek

    // EKLENDİ: Render döngüsü ve istemci yönetimi için
    struct wl_list views; // struct termuxwc_view listesi
    struct wl_listener new_xdg_surface;
    struct wl_listener output_frame;
};

static void *vnc_thread_fn(void *arg) {
    rfbScreenInfoPtr rfb = (rfbScreenInfoPtr)arg;
    /* VNC için engelleyici olay döngüsü */
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

    rfb->serverFormat.bitsPerPixel = 32;
    rfb->serverFormat.depth        = 24;
    rfb->serverFormat.bigEndian    = FALSE;
    rfb->serverFormat.trueColour   = TRUE;
    rfb->serverFormat.redMax   = 255;
    rfb->serverFormat.greenMax = 255;
    rfb->serverFormat.blueMax  = 255;
    rfb->serverFormat.redShift   = 16;
    rfb->serverFormat.greenShift = 8;
    rfb->serverFormat.blueShift  = 0;

    rfbInitServer(rfb);
    server->vnc = rfb;

    if (pthread_create(&server->vnc_thread, NULL, vnc_thread_fn, rfb) != 0) {
        wlr_log(WLR_ERROR, "Failed to create VNC thread");
        rfbScreenCleanup(rfb);
        return -1;
    }

    wlr_log(WLR_INFO, "VNC server listening on port %d (display :%d)", rfb->port, rfb->port - 5900);
    return 0;
}

static void copy_pixels_to_vnc(struct termuxwc_server *s, int width, int height) {
    if (!s->vnc) return;
    rfbScreenInfoPtr rfb = s->vnc;
    if (!rfb->frameBuffer) return;

    size_t fb_bytes = (size_t)width * (size_t)height * 4;
    memcpy(rfb->frameBuffer, s->pixel_buffer, fb_bytes);

    rfbMarkRectAsModified(rfb, 0, 0, width, height);
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

// GÜNCELLENDİ: Render döngüsü (frame) işleyicisi (wlroots-0.20 API V7)
static void handle_output_frame(struct wl_listener *listener, void *data) {
    struct termuxwc_server *server = wl_container_of(listener, server, output_frame);
    struct wlr_output *output = data;
    struct wlr_renderer *renderer = server->renderer;

    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);

    // YENİ API (Output V4 / Render V7)
    // wlr_output_state, commit edilecek yeni durumu tanımlar
    struct wlr_output_state state;
    wlr_output_state_init(&state);
    wlr_output_state_set_enabled(&state, true); // Çıktıyı etkinleştir

    // Çıktıdan çizim yapmak için bir buffer al
    struct wlr_buffer *buffer = wlr_output_acquire_buffer(output, &state);
    if (!buffer) {
        wlr_output_state_finish(&state);
        return;
    }

    // ESKİ: wlr_output_attach_render(...)
    // YENİ: wlr_renderer_begin_with_buffer(...)
    if (!wlr_renderer_begin_with_buffer(renderer, buffer)) {
        wlr_buffer_unlock(buffer); // Hata durumunda buffer'ı bırak
        wlr_output_state_finish(&state);
        return;
    }

    // Arka planı temizle
    float color[4] = {0.1f, 0.1f, 0.1f, 1.0f};
    wlr_renderer_clear(renderer, color);

    struct termuxwc_view *view;
    wl_list_for_each(view, &server->views, link) {
        struct wlr_xdg_surface *xdg_surface = view->xdg_surface;
        if (!xdg_surface->surface->mapped) {
            continue;
        }

        struct wlr_texture *texture = wlr_surface_get_texture(xdg_surface->surface);
        if (!texture) {
            continue;
        }

        struct wlr_box geo_box;
        wlr_xdg_surface_get_geometry(xdg_surface, &geo_box);
        
        // DÜZELTİLDİ: Konumu toplevel->current.x yerine view->x'den al
        double view_x = view->x + geo_box.x;
        double view_y = view->y + geo_box.y;

        double ox = 0, oy = 0;
        wlr_output_layout_output_coords(server->output_layout, output, &ox, &oy);

        struct wlr_box render_box = {
            .x = view_x - ox,
            .y = view_y - oy,
            .width = geo_box.width,
            .height = geo_box.height,
        };

        // ESKİ: wlr_output_matrix(...)
        // YENİ: wlr_output_state_get_matrix(...)
        float matrix[9];
        wlr_output_state_get_matrix(&state, &matrix); // Projeksiyon matrisini 'state' üzerinden al

        float final_matrix[9];
        wlr_matrix_project_box(&final_matrix, &render_box, 
                               WL_OUTPUT_TRANSFORM_NORMAL, 0, &matrix);

        wlr_render_texture_with_matrix(renderer, texture, &final_matrix, 1.0f);
        wlr_surface_send_frame_done(xdg_surface->surface, &now);
    }

    // ESKİ: wlr_renderer_end(renderer);
    // YENİ:
    wlr_renderer_end(renderer); // Render işlemini bitir

    // ESKİ: wlr_output_commit(output);
    // YENİ: Buffer'ı state'e ata ve state'i commit et
    wlr_output_state_set_buffer(&state, buffer);
    if (!wlr_output_commit_state(output, &state)) {
        wlr_buffer_unlock(buffer);
        wlr_output_state_finish(&state);
        return;
    }
    
    // DÜZELTİLDİ: YENİ Piksel okuma API'si ve 'pixel' -> 'pixel_buffer' hatası
    if (!wlr_buffer_read_pixels(buffer, WL_SHM_FORMAT_XRGB8888, NULL,
                                 output->width * 4, // stride
                                 output->width, output->height,
                                 0, 0, 0, 0, // src_x, src_y, dst_x, dst_y
                                 server->pixel_buffer)) {
        
        wlr_log(WLR_ERROR, "Failed to read pixels");
    } else {
        copy_pixels_to_vnc(server, output->width, output->height);
    }

    wlr_buffer_unlock(buffer); // Buffer'ı serbest bırak
    wlr_output_state_finish(&state); // State'i temizle
}

// EKLENDİ: İstemci "commit" yaptığında (yeni kare hazır olduğunda) tetiklenir
static void handle_surface_commit(struct wl_listener *listener, void *data) {
    struct termuxwc_view *view = wl_container_of(listener, view, commit);
    
    // Tüm çıktılarda yeni bir kare çizilmesini planla
    struct wlr_output *output;
    wlr_output_layout_for_each_output(view->server->output_layout, output) {
        wlr_output_schedule_frame(output);
    }
}

// EKLENDİ: Pencere (view) yok edildiğinde listeyi temizle
static void handle_view_destroy(struct wl_listener *listener, void *data) {
    struct termuxwc_view *view = wl_container_of(listener, view, destroy);
    wl_list_remove(&view->commit.link);
    wl_list_remove(&view->map.link);
    wl_list_remove(&view->unmap.link);
    wl_list_remove(&view->destroy.link);
    wl_list_remove(&view->link);
    free(view);
}

// EKLENDİ: Pencere haritalandığında (görünür olduğunda)
static void handle_view_map(struct wl_listener *listener, void *data) {
    struct termuxwc_view *view = wl_container_of(listener, view, map);
    
    // Pencereyi ortala (basit bir yerleşim)
    struct wlr_box geo_box;
    wlr_xdg_surface_get_geometry(view->xdg_surface, &geo_box);
    
    // DÜZELTİLDİ: Konumu toplevel->current yerine kendi struct'ımızda saklıyoruz
    view->x = (view->server->width - geo_box.width) / 2;
    view->y = (view->server->height - geo_box.height) / 2;
    
    // Yeni kare çiz
    handle_surface_commit(&view->commit, NULL);
}

// EKLENDİ: Pencere haritadan kaldırıldığında (gizlendiğinde)
static void handle_view_unmap(struct wl_listener *listener, void *data) {
    struct termuxwc_view *view = wl_container_of(listener, view, unmap);
    // Değişikliği yansıtmak için yeni kare çiz
    handle_surface_commit(&view->commit, NULL);
}

// EKLENDİ: Yeni bir XDG yüzeyi (pencere) bağlandığında
static void handle_new_xdg_surface(struct wl_listener *listener, void *data) {
    struct termuxwc_server *server = wl_container_of(listener, server, new_xdg_surface);
    struct wlr_xdg_surface *xdg_surface = data;

    if (xdg_surface->role != WLR_XDG_SURFACE_ROLE_TOPLEVEL) {
        return; // Sadece ana pencerelerle (toplevel) ilgilen
    }

    struct termuxwc_view *view = calloc(1, sizeof(struct termuxwc_view));
    view->server = server;
    view->xdg_surface = xdg_surface;
    wl_list_insert(&server->views, &view->link);

    // Olay dinleyicilerini (listener) bağla
    view->commit.notify = handle_surface_commit;
    wl_signal_add(&xdg_surface->surface->events.commit, &view->commit);
    view->map.notify = handle_view_map;
    wl_signal_add(&xdg_surface->events.map, &view->map);
    view->unmap.notify = handle_view_unmap;
    wl_signal_add(&xdg_surface->events.unmap, &view->unmap);
    view->destroy.notify = handle_view_destroy;
    wl_signal_add(&xdg_surface->events.destroy, &view->destroy);
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

    // EKLENDİ: View listesini başlat
    wl_list_init(&server.views);

    if (!wlr_shm_create(server.display, 1, (const uint32_t[]){  WL_SHM_FORMAT_XRGB8888, WL_SHM_FORMAT_ARGB8888 }, 2)) {
        wlr_log(WLR_ERROR, "Failed to create wl_shm global");
        // Devam et
    } else {
        wlr_log(WLR_INFO, "wl_shm global created");
    }

    server.backend = wlr_headless_backend_create(server.event_loop);
    if (!server.backend) {
        wlr_log(WLR_ERROR, "Failed to create headless backend");
        goto fail_display;
    }

    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "Failed to create renderer");
        goto fail_backend;
    }

    server.compositor = wlr_compositor_create(server.display, 5, server.renderer);
    if (!server.compositor) {
        wlr_log(WLR_ERROR, "Failed to create compositor");
        goto fail_renderer;
    }

    server.xdg_shell = wlr_xdg_shell_create(server.display, 3);
    if (!server.xdg_shell) {
        wlr_log(WLR_ERROR, "Failed to create xdg-shell");
        goto fail_compositor;
    }

    // EKLENDİ: Yeni xdg_surface olaylarını dinle
    server.new_xdg_surface.notify = handle_new_xdg_surface;
    wl_signal_add(&server.xdg_shell->events.new_surface, &server.new_xdg_surface);


    server.output_layout = wlr_output_layout_create(server.display);
    if (!server.output_layout) {
        wlr_log(WLR_ERROR, "Failed to create output layout");
        goto fail_xdg;
    }

    server.width  = 1280;
    server.height = 720;

    server.pixel_buffer = calloc((size_t)server.width * server.height, 4);
    if (!server.pixel_buffer) {
        wlr_log(WLR_ERROR, "Failed to allocate pixel_buffer");
        goto fail_xdg;
    }

    if (start_vnc_server(&server, server.width, server.height) < 0) {
        goto fail_pixel_buffer;
    }

    // Backend'i başlat
    if (!wlr_backend_start(server.backend)) {
        wlr_log(WLR_ERROR, "Failed to start backend");
        goto fail_vnc;
    }

    // EKLENDİ: Headless backend için manuel olarak bir çıktı (output) oluştur
    struct wlr_output *output = wlr_headless_output_create(server.backend);
    if (!output) {
        wlr_log(WLR_ERROR, "Failed to create headless output");
        goto fail_vnc;
    }
    wlr_output_set_name(output, "VNC-1");
    // Modu ayarla (genişlik, yükseklik, 0Hz -> manuel frame tetikleme)
    wlr_output_set_custom_mode(output, server.width, server.height, 0); 
    wlr_output_layout_add_auto(server.output_layout, output);
    wlr_output_create_global(output, server.display);

    // EKLENDİ: Çıktının frame olayını render döngümüze bağla
    server.output_frame.notify = handle_output_frame;
    wl_signal_add(&output->events.frame, &server.output_frame);

    // EKLENDİ: Çıktıyı etkinleştir ve ilk durumu commit et
    // wlroots 0.17+ 'wlr_output_enable' gerektirir.
    // 0.20'de, bu commit ile yapılır, ancak biz yine de çağırıyoruz.
    if (!wlr_output_enable(output, true)) {
         wlr_log(WLR_ERROR, "Failed to enable output");
         goto fail_vnc;
    }
    
    // wlroots 0.20+ 'wlr_output_commit_state' gerektirir.
    // Başlangıçta boş bir state commit edelim.
    struct wlr_output_state initial_state;
    wlr_output_state_init(&initial_state);
    wlr_output_state_set_enabled(&initial_state, true);
    if (!wlr_output_commit_state(output, &initial_state)) {
         wlr_log(WLR_ERROR, "Failed to commit initial output state");
         wlr_output_state_finish(&initial_state);
         goto fail_vnc;
    }
    wlr_output_state_finish(&initial_state);


    // Wayland soketini oluştur
    const char *socket = wl_display_add_socket_auto(server.display);
    if (!socket) {
        wlr_log(WLR_ERROR, "Failed to create Wayland socket");
        goto fail_vnc;
    }
    setenv("WAYLAND_DISPLAY", socket, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket);

    const char *xdg = getenv("XDG_RUNTIME_DIR");
    if (!xdg) {
        xdg = "/data/data/com.termux/files/usr/tmp/wayland";
        setenv("XDG_RUNTIME_DIR", xdg, 1);
        mkdir(xdg, 0700);
    }

    pid_t term_pid = launch_pty_terminal(socket, xdg);
    if (term_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to launch terminal");
    } else {
        wlr_log(WLR_INFO, "Launched terminal (PID %d)", term_pid);
    } 
  
    wl_display_run(server.display);

    /* Cleanup */
    // TODO: VNC thread'ini güvenli bir şekilde durdur
    rfbScreenCleanup(server.vnc);
    
fail_vnc:
    // ...
fail_pixel_buffer:
    free(server.pixel_buffer);
fail_xdg:
    wlr_output_layout_destroy(server.output_layout);
    wlr_xdg_shell_destroy(server.xdg_shell);
fail_compositor:
    wlr_compositor_destroy(server.compositor);
fail_renderer:
    if (server.renderer)	      
        wlr_renderer_destroy(server.renderer);
fail_backend:
    if (server.backend)
        wlr_backend_destroy(server.backend);
fail_display:
    if (server.display)
        wl_display_destroy_clients(server.display);
        wl_display_destroy(server.display);
    return 1;
}

