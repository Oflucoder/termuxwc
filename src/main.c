/*
 * termuxwc: wlroots-0.20-dev tabanlı, 'wayvnc' gibi istemcilerle
 * çalışan basit bir Termux Wayland compositor'ü.
 */

#define _POSIX_C_SOURCE 200809L
#include <limits.h> // For PATH_MAX
#include <sys/stat.h> // For stat(), mkdir()
#include <sys/wait.h> // For waitpid(), WNOHANG
#include <errno.h>    // For errno
#include <unistd.h>   // For usleep(), forkpty()
#include <pty.h>      // For forkpty()
#include <termios.h>  // For cfmakeraw(), tcgetattr()
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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
// #include <wlr/types/wlr_allocator.h> // *** KALDIRILDI: Bu dosya sisteminizde yok ***
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_shm.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_surface.h>
// wayvnc gibi VNC istemcileri için gerekli protokoller
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/matrix.h> 

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
    // struct wlr_allocator *allocator; // *** KALDIRILDI ***
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
// Updated signature to take socket name and xdg_runtime_dir explicitly
static pid_t launch_pty_terminal(const char *socket_name, const char *xdg_runtime_dir) {
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
        // Set environment variables for the child process
        setenv("WAYLAND_DISPLAY", socket_name, 1); // Use the socket name provided
        setenv("XDG_RUNTIME_DIR", xdg_runtime_dir, 1); // Use the verified runtime dir provided
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
        wlr_output_schedule_frame(output);
        return;
    }

    // 🔑 CRITICAL: Tell wlroots the frame was presented for screencopy
    // Populate the present event structure.
    //
    // *** DERLEYİCİ HATASI DÜZELTMESİ: ***
    // Derleyici .when alanının 'time_t' (long) olmasını bekliyor, 'struct timespec *' değil.
    //
    struct wlr_output_event_present present_event = {
        .when = now.tv_sec, // 'time_t' (saniye) değeri
        .seq = 0,
        .flags = WLR_OUTPUT_PRESENT_VSYNC, 
    };

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

    // --- Gelişmiş Loglama ve Tampon Kapatma ---
    // Logların WAYLAND_DEBUG=1 ile bile görünmesini sağlamak için
    setenv("WLR_LOG_LEVEL", "DEBUG", 1);
    wlr_log_init(WLR_DEBUG, NULL); // Logların stderr'e gitmesini zorla
    setbuf(stdout, NULL); // stdout tamponlamasını kapat
    setbuf(stderr, NULL); // stderr tamponlamasını kapat
    // --- Bitiş ---

    struct termuxwc_server server = {0};

    // --- SET UP ENVIRONMENT EARLY ---
    // *** DÜZELTME: ***
    // `mkdir` komutunun "No such file or directory" hatasını düzeltmek için
    // önce üst dizini (.../tmp) oluşturmayı deneyeceğiz.
    char xdg_runtime_dir_path[PATH_MAX];
    char tmp_path[PATH_MAX]; // Üst dizin için
    const char *prefix = getenv("PREFIX");
    if (!prefix) {
        prefix = "/data/data/com.termux/files/usr"; // Default Termux prefix
    }
    
    // Path 1: Üst dizin (.../tmp)
    snprintf(tmp_path, sizeof(tmp_path), "%s/tmp", prefix);
    
    // Path 2: Asıl dizin (.../tmp/termuxwc_runtime)
    snprintf(xdg_runtime_dir_path, sizeof(xdg_runtime_dir_path), "%s/tmp/termuxwc_runtime", prefix);

    // Adım 1: Üst dizini oluşturmayı dene
    if (mkdir(tmp_path, 0700) != 0 && errno != EEXIST) {
         // Başarısız olursa logla, ama yine de devam etmeyi dene
         // *** HATA DÜZELTMESİ: WLR_WARN -> WLR_INFO ***
         wlr_log(WLR_INFO, "Could not create parent runtime dir %s: %s. Continuing anyway...", tmp_path, strerror(errno));
    } else {
         wlr_log(WLR_DEBUG, "Ensured parent runtime dir exists: %s", tmp_path);
    }

    // Adım 2: Asıl dizini oluşturmayı dene
    if (mkdir(xdg_runtime_dir_path, 0700) != 0 && errno != EEXIST) {
         // Bu, kullanıcının gördüğü hataydı (ENOENT)
         wlr_log(WLR_ERROR, "Failed to create XDG_RUNTIME_DIR %s: %s", xdg_runtime_dir_path, strerror(errno));
         // Bu daha ciddi bir sorun, ancak devam etmeyi deneyeceğiz.
    } else {
         wlr_log(WLR_INFO, "Ensured XDG_RUNTIME_DIR exists: %s", xdg_runtime_dir_path);
    }
    
    // Değişkeni bu process ve tüm alt process'ler (fork/exec) için ayarla.
    setenv("XDG_RUNTIME_DIR", xdg_runtime_dir_path, 1); 
    const char *final_xdg_runtime_dir = xdg_runtime_dir_path;
    wlr_log(WLR_INFO, "Forcing XDG_RUNTIME_DIR to: %s", final_xdg_runtime_dir);
    // --- Önceki XDG_RUNTIME_DIR mantığı değiştirildi ---


    // Pencereleri tutacağımız listeyi başlat
    wl_list_init(&server.views);

    // *** ÇÖKME DÜZELTMESİ: 'gles2' yerine 'pixman'e geri dön ***
    // EGL/GLES2 sürücüleri Termux'ta başarısız oluyor.
    setenv("WLR_RENDERER", "pixman", 1);
    // setenv("WLR_RENDERER", "gles2", 1); // ESKİ SATIR

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

    // Renderer (çizim motoru) - şimdi pixman'i deneyecek
    server.renderer = wlr_renderer_autocreate(server.backend);
    if (!server.renderer) {
        wlr_log(WLR_ERROR, "Failed to create renderer (pixman).");
        goto fail_backend;
    }

    // *** DÜZELTME (SATIR SIRASI): 'wlr_renderer_init_wl_display' buraya taşındı ***
    // Renderer oluşturulduktan hemen sonra ve SHM'den önce.
    wlr_renderer_init_wl_display(server.renderer, server.display);


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
    // *** DÜZELTME: 'wlr_shm_create_with_renderer' ÇAĞRISI GERİ EKLENDİ ***
    // 'already registered' logu önemli değil. Bu çağrı, 
    // allocator'ü düzgün bir şekilde başlatmak için GEREKLİ.
    if (!wlr_shm_create_with_renderer(server.display, 1, server.renderer)) {
        wlr_log(WLR_ERROR, "SHM creation failed");
    }

    // *** 'wlr_renderer_init_wl_display' çağrısı buradan KALDIRILDI (yukarı taşındı) ***


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
    server.output->name = "VNC-1"; // wayvnc'nin bulabilmesi için bir isim ver

    // *** YENİ DÜZELTME (wlroots-0.20-dev API): Çıktıyı "State" API'si ile etkinleştir ***
    // wayvnc'nin "failed()" hatası vermemesi için bu gerekli.
    struct wlr_output_state state;
    wlr_output_state_init(&state); // Boş state'i başlat
    
    // State üzerine özel modu ayarla
    wlr_output_state_set_custom_mode(&state, width, height, 0); // Genişlik, yükseklik, hz (0=varsayılan)
    
    // State üzerine etkinleştirme durumunu ayarla
    wlr_output_state_set_enabled(&state, true);

    // Yeni state'i çıktıya atomik olarak uygula
    if (!wlr_output_commit_state(server.output, &state)) {
         wlr_log(WLR_ERROR, "Failed to commit headless output state");
         wlr_output_state_finish(&state); // Başarısız olsa bile state'i temizle
         goto fail_layout;
    }
    
    // Başarılı commit'ten sonra state'i temizle
    wlr_output_state_finish(&state);
    // *** DÜZELTME SONU ***

    // Render döngüsü için 'frame' sinyalini dinle
    server.output_frame.notify = handle_output_frame;
    wl_signal_add(&server.output->events.frame, &server.output_frame);

    wlr_output_layout_add_auto(server.output_layout, server.output);

    // Başlangıçta bir kare çiz
    wlr_output_schedule_frame(server.output);

    // Wayland soketini oluştur (this MUST happen AFTER display creation)
    const char *socket_name = wl_display_add_socket_auto(server.display);
    if (!socket_name) {
        wlr_log(WLR_ERROR, "Failed to create Wayland socket");
        
        // *** DÜZELTME (Crash Koruması): ***
        // Soket oluşturma başarısız olursa, çıkış (output) 
        // 'frame' dinleyicisini (listener) goto fail_layout'a gitmeden
        // önce manuel olarak kaldırmalıyız.
        // Yoksa wlr_backend_destroy() -> wlr_output_finish() 
        // içinde assertion failure hatası alırız.
        wl_list_remove(&server.output_frame.link);

        goto fail_layout; // Şimdi atlamak güvenli
    }
    // Set WAYLAND_DISPLAY environment variable for child processes (like foot)
    setenv("WAYLAND_DISPLAY", socket_name, 1);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s", socket_name);
    wlr_log(WLR_INFO, "WAYLAND_DISPLAY=%s (Full path likely $XDG_RUNTIME_DIR/%s)", socket_name, socket_name);

    // --- PID Değişkenlerini Başlat ---
    pid_t xwayland_pid = 0;
    pid_t term_pid = 0;
    pid_t wayvnc_pid = 0; // wayvnc'yi takip etmek için PID eklendi

    // --- TÜM HATALI Xvfb / Xvnc / xterm BAŞLATMA MANTIĞI KALDIRILDI ---
    // Bu mantık hem hatalıydı (display çakışması) hem de 
    // Wayland compositor'ün amacına uygun değildi.
    // VNC sunucusu (wayvnc) ayrı bir istemci olarak çalıştırılmalıdır.


    // --- Start xwayland ---
    wlr_log(WLR_INFO, "Starting xwayland...");
    xwayland_pid = fork();
    if (xwayland_pid == 0) {
        // Child process: exec xwayland
        setenv("WAYLAND_DISPLAY", socket_name, 1); // Use the socket name from wl_display_add_socket_auto
        setenv("XDG_RUNTIME_DIR", final_xdg_runtime_dir, 1); // Use the verified XDG_RUNTIME_DIR
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


    // Terminali (foot) başlat
    // Use the verified socket name and XDG_RUNTIME_DIR
    term_pid = launch_pty_terminal(socket_name, final_xdg_runtime_dir);
    if (term_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to launch terminal");
    } else {
        wlr_log(WLR_INFO, "Launched terminal (PID %d)", term_pid);
    }

    // --- wayvnc'yi otomatik başlat ---
    wlr_log(WLR_INFO, "Starting wayvnc...");
    wayvnc_pid = fork();
    if (wayvnc_pid == 0) {
        // Çocuk process: exec wayvnc
        // WAYLAND_DISPLAY ve XDG_RUNTIME_DIR zaten ebeveynden miras alınır.
        execl("/data/data/com.termux/files/usr/bin/wayvnc", "wayvnc", "0.0.0.0", (char *)NULL);
        perror("wayvnc exec failed");
        _exit(1);
    } else if (wayvnc_pid < 0) {
        wlr_log(WLR_ERROR, "Failed to fork for wayvnc");
        // Ölümcül değil, compositor VNC olmadan çalışmaya devam eder
    } else {
        wlr_log(WLR_INFO, "wayvnc started (PID %d). VNC available on 0.0.0.0:5900", wayvnc_pid);
    }


    wlr_log(WLR_INFO, "TermuxWC running. VNC should be available on port 5900.");


    // Olay döngüsünü çalıştır
    wl_display_run(server.display);

    //
    // Cleanup (Hata ayıklama etiketleri)
    //
    // Kill child processes on exit if necessary
    if (xwayland_pid > 0) { 
        wlr_log(WLR_INFO, "Terminating Xwayland (PID %d)...", xwayland_pid);
        kill(xwayland_pid, SIGTERM);
    }
    if (term_pid > 0) {
        wlr_log(WLR_INFO, "Terminating terminal (PID %d)...", term_pid);
        kill(term_pid, SIGTERM);
    }
    if (wayvnc_pid > 0) { 
        wlr_log(WLR_INFO, "Terminating wayvnc (PID %d)...", wayvnc_pid);
        kill(wayvnc_pid, SIGTERM);
    }

fail_seat:
    if (server.seat) wlr_seat_destroy(server.seat);
fail_layout:
    if (server.output_layout) wlr_output_layout_destroy(server.output_layout);
fail_xdg:
fail_compositor:
fail_renderer:
    // *** 'wlr_allocator_destroy' çağrısı buradan KALDIRILDI ***
    if (server.renderer) wlr_renderer_destroy(server.renderer);
fail_backend:
    if (server.backend) wlr_backend_destroy(server.backend);
fail_display:
    if (server.display) wl_display_destroy(server.display);
    return 1;
}


