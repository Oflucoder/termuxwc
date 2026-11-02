/*
 * termuxwc — lightweight Sway-style tiling Wayland compositor for
 * Termux/Android (headless + VNC) and native Linux desktops.
 *
 * Entry point: backend/renderer bootstrap, global protocol registration
 * and the main event loop. Compositor logic lives in the sibling modules:
 *   src/config.c   – configuration file loading & app launching
 *   src/layout.c   – dynamic master-stack tiling engine
 *   src/xdg.c      – XDG shell windows, workspaces, focus
 *   src/cursor.c   – pointer interaction, interactive move/resize
 *   src/inputs.c   – keyboards, virtual input devices, keybindings
 *   src/layers.c   – layer-shell (bars / wallpapers)
 *   src/outputs.c  – displays & frame scheduling
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"
#include "termuxwc/config.h"

#include <getopt.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include <wayland-server-core.h>

#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/backend/multi.h>
#ifdef WLR_BACKEND_DRM
#include <wlr/backend/drm.h>
#endif
#include <wlr/backend/wayland.h>
#ifdef WLR_BACKEND_X11
#include <wlr/backend/x11.h>
#endif
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_data_device.h>
#if WLR_VERSION_MAJOR > 0 || (WLR_VERSION_MAJOR == 0 && WLR_VERSION_MINOR >= 17)
#include <wlr/types/wlr_fractional_scale_v1.h>
#define TWC_HAS_FRACTIONAL_SCALE 1
#endif
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/util/log.h>

static void handle_terminate_signal(int sig) {
	(void)sig;
}

/*
 * Ensure a usable XDG_RUNTIME_DIR. On Termux/Android the standard path does
 * not exist, so fall back to the app-private tmp directory.
 */
static void setup_xdg_runtime_dir(void) {
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir || access(runtime_dir, W_OK) != 0) {
#ifdef __ANDROID__
		const char *fallback = "/data/data/com.termux/files/usr/tmp/xdg-runtime";
#else
		const char *fallback = "/tmp/termuxwc-xdg-runtime";
#endif
		mkdir(fallback, 0700);
		setenv("XDG_RUNTIME_DIR", fallback, 1);
		wlr_log(WLR_INFO, "XDG_RUNTIME_DIR configured: %s", fallback);
	}
}

int main(int argc, char *argv[]) {
	const char *log_env = getenv("WLR_LOG_LEVEL");
	enum wlr_log_importance log_level = WLR_ERROR;
	if (log_env) {
		if (strcmp(log_env, "debug") == 0) log_level = WLR_DEBUG;
		else if (strcmp(log_env, "info") == 0) log_level = WLR_INFO;
	}
	wlr_log_init(log_level, NULL);

	struct termuxwc_server server = {0};
	server.default_width = 1280;
	server.default_height = 720;
	server.default_refresh_rate = 120000;
	const char *env_refresh = getenv("TERMUXWC_REFRESH_RATE");
	if (env_refresh && atoi(env_refresh) > 0) {
		server.default_refresh_rate = atoi(env_refresh) * 1000;
	}
	server.active_workspace = 1;

	for (int i = 0; i < NUM_WORKSPACES; i++) {
		server.workspaces[i].id = i + 1;
		wl_list_init(&server.workspaces[i].toplevels);
		server.workspaces[i].focused = NULL;
	}

	twc_load_or_create_config(&server.config);

	char *startup_cmd = NULL;
	char *backend_name = NULL;
	bool force_headless = false;
	char *custom_renderer = NULL;

	int opt;
	while ((opt = getopt(argc, argv, "s:w:h:r:HR:b:")) != -1) {
		switch (opt) {
		case 's':
			startup_cmd = optarg;
			break;
		case 'w':
			server.default_width = atoi(optarg);
			if (server.default_width <= 0) server.default_width = 1280;
			break;
		case 'h':
			server.default_height = atoi(optarg);
			if (server.default_height <= 0) server.default_height = 720;
			break;
		case 'r':
			server.default_refresh_rate = atoi(optarg) * 1000;
			if (server.default_refresh_rate <= 0) server.default_refresh_rate = 120000;
			break;
		case 'H':
			force_headless = true;
			break;
		case 'R':
			custom_renderer = optarg;
			break;
		case 'b':
			backend_name = optarg; // auto | drm | wayland | x11 | headless
			break;
		default:
			fprintf(stderr,
				"Usage: %s [-s <cmd>] [-w <width>] [-h <height>] [-r <fps>]\n"
				"       [-H (headless)] [-R <auto|gles2|vulkan|pixman>]"
				" [-b <auto|drm|wayland|x11|headless>]\n", argv[0]);
			return 1;
		}
	}

	if (custom_renderer) {
		setenv("WLR_RENDERER", custom_renderer, 1);
	}

	setup_xdg_runtime_dir();

	server.wl_display = wl_display_create();
	if (!server.wl_display) {
		wlr_log(WLR_ERROR, "Failed to create Wayland display");
		return 1;
	}
	server.event_loop = wl_display_get_event_loop(server.wl_display);

	if (!getenv("WLR_RENDERER")) {
		setenv("WLR_RENDERER", "auto", 0);
	}

	// --- Backend selection (Linux port / HW acceleration paths) ---
	// Inside Termux there is no parent display server, so autocreate falls
	// back to the headless backend immediately (VNC via wayvnc). On real
	// Linux: "-b drm" gives direct GPU rendering with KMS page flips (no
	// VNC at all); otherwise we prefer nested Wayland (works under any
	// session), then X11, then headless. Explicit choice via -b <backend>.
	if (!force_headless) {
#ifdef WLR_BACKEND_DRM
		if (backend_name && strcmp(backend_name, "drm") == 0) {
			// Native DRM/GBM backend: the compositor renders directly on the
			// physical GPU and presents via KMS page flips. Needs a free DRM
			// device (root or video/render group), no parent display server.
			server.backend = wlr_drm_backend_create(server.event_loop, NULL, NULL);
			if (!server.backend) {
				wlr_log(WLR_ERROR, "DRM backend unavailable"
					" (no free /dev/dri device or insufficient permissions)");
				return 1;
			}
		} else
#endif
		if (backend_name && strcmp(backend_name, "wayland") == 0 &&
		    getenv("WAYLAND_DISPLAY")) {
			server.backend = wlr_wl_backend_create(server.event_loop, NULL);
		}
#ifdef WLR_BACKEND_X11
		else if (backend_name && strcmp(backend_name, "x11") == 0 &&
		         getenv("DISPLAY")) {
			server.backend = wlr_x11_backend_create(server.event_loop, NULL);
		}
#endif
		else if (backend_name && strcmp(backend_name, "headless") == 0) {
			server.backend = NULL; // handled by fallback below
		} else {
			server.backend = wlr_backend_autocreate(server.event_loop, NULL);
		}
	}
	if (server.backend == NULL) {
		if (backend_name) {
			wlr_log(WLR_INFO, "Backend '%s' not available; falling back to headless",
				backend_name);
		}
		wlr_log(WLR_INFO, "Creating headless backend (%dx%d)...",
			server.default_width, server.default_height);
		server.backend = wlr_headless_backend_create(server.event_loop);
		if (server.backend == NULL) {
			wlr_log(WLR_ERROR, "Failed to create headless backend");
			return 1;
		}
		wlr_headless_add_output(server.backend,
			server.default_width, server.default_height);
	}

	server.renderer = wlr_renderer_autocreate(server.backend);
	if (!server.renderer) {
		const char *req = getenv("WLR_RENDERER");
		wlr_log(WLR_INFO, "Renderer '%s' not available headlessly; activating Pixman software renderer",
			req ? req : "auto");
		server.renderer = wlr_pixman_renderer_create();
	}
	if (!server.renderer) {
		wlr_log(WLR_ERROR, "Failed to create any wlr_renderer");
		return 1;
	}

	wlr_renderer_init_wl_display(server.renderer, server.wl_display);
	wlr_linux_dmabuf_v1_create_with_renderer(server.wl_display, 4, server.renderer);

	server.allocator = wlr_allocator_autocreate(server.backend, server.renderer);
	if (!server.allocator) {
		wlr_log(WLR_ERROR, "Failed to create wlr_allocator");
		return 1;
	}

	// Wayland protocol managers
	wlr_compositor_create(server.wl_display, 6, server.renderer);
	wlr_subcompositor_create(server.wl_display);
	wlr_data_device_manager_create(server.wl_display);

	wlr_screencopy_manager_v1_create(server.wl_display);

	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	wlr_xdg_activation_v1_create(server.wl_display);
#ifdef TWC_HAS_FRACTIONAL_SCALE
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
#endif
	wlr_viewporter_create(server.wl_display);
	wlr_xdg_decoration_manager_v1_create(server.wl_display);
	wlr_text_input_manager_v3_create(server.wl_display);
	wlr_presentation_create(server.wl_display, server.backend, 1);

	// Output layout & Scene graph
	server.output_layout = wlr_output_layout_create(server.wl_display);
	setenv("WLR_SCENE_DISABLE_DIRECT_SCANOUT", "1", 0);
	server.scene = wlr_scene_create();
	server.scene_layout = wlr_scene_attach_output_layout(
		server.scene, server.output_layout);

	// Layer order: background -> bottom -> windows -> top -> overlay
	server.scene_background = wlr_scene_tree_create(&server.scene->tree);
	server.scene_bottom = wlr_scene_tree_create(&server.scene->tree);
	server.scene_windows = wlr_scene_tree_create(&server.scene->tree);
	server.scene_top = wlr_scene_tree_create(&server.scene->tree);
	server.scene_overlay = wlr_scene_tree_create(&server.scene->tree);

	server.background_rect = wlr_scene_rect_create(
		server.scene_background,
		server.default_width, server.default_height,
		server.config.color_background);

	// Subsystem wiring (see respective modules)
	twc_setup_outputs(&server);
	twc_setup_xdg_shell(&server);
	twc_setup_layer_shell(&server);

	// Pointer & Cursor
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.seat = wlr_seat_create(server.wl_display, "seat0");
	twc_setup_cursor_listeners(&server);
	twc_setup_input(&server);

	if (!wlr_backend_start(server.backend)) {
		wlr_log(WLR_ERROR, "Failed to start backend");
		return 1;
	}

	const char *socket = wl_display_add_socket_auto(server.wl_display);
	if (!socket) {
		wlr_log(WLR_ERROR, "Failed to create socket");
		return 1;
	}

	setenv("WAYLAND_DISPLAY", socket, 1);
	const char *xdg_runtime = getenv("XDG_RUNTIME_DIR");
	wlr_log(WLR_INFO, "termuxwc tiling compositor active on WAYLAND_DISPLAY=%s", socket);
	fprintf(stderr, "Wayland socket path: %s/%s\n", xdg_runtime, socket);
	fflush(stderr);

	pid_t child_pid = -1;
	if (startup_cmd) {
		child_pid = twc_launch_command(startup_cmd, socket);
	}

	struct sigaction sa;
	sa.sa_handler = handle_terminate_signal;
	sigemptyset(&sa.sa_mask);
	sa.sa_flags = 0;
	sigaction(SIGINT, &sa, NULL);
	sigaction(SIGTERM, &sa, NULL);

	wl_display_run(server.wl_display);

	if (child_pid > 0) {
		kill(child_pid, SIGTERM);
		waitpid(child_pid, NULL, 0);
	}

	wl_display_destroy_clients(server.wl_display);
	wl_list_remove(&server.new_input.link);
	wl_list_remove(&server.new_output.link);
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_output_layout_destroy(server.output_layout);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);

	return 0;
}
