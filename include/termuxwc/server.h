#ifndef TERMUXWC_SERVER_H
#define TERMUXWC_SERVER_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-server-core.h>
#include <wlr/util/box.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>

#define NUM_WORKSPACES 9

enum termuxwc_cursor_mode {
	TERMUXWC_CURSOR_PASSTHROUGH,
	TERMUXWC_CURSOR_MOVE,
	TERMUXWC_CURSOR_RESIZE,
};

struct termuxwc_server;

struct termuxwc_config {
	uint32_t mod_mask; // WLR_MODIFIER_LOGO or WLR_MODIFIER_ALT
	int inner_gap;
	int outer_gap;
	int border_width;
	char terminal_cmd[256];
	char launcher_cmd[256];
	float color_active_border[4];
	float color_inactive_border[4];
	float color_background[4];
	char background_image[512]; // optional PNG wallpaper path (empty = solid color)
};

struct termuxwc_toplevel {
	struct wl_list link; // workspace link
	struct termuxwc_server *server;
	struct wlr_xdg_toplevel *xdg_toplevel;
	struct wlr_scene_tree *scene_tree;

	int workspace_id; // 1 to NUM_WORKSPACES
	bool is_floating;
	bool is_fullscreen;

	// Colored window borders
	struct wlr_scene_rect *border_top;
	struct wlr_scene_rect *border_bottom;
	struct wlr_scene_rect *border_left;
	struct wlr_scene_rect *border_right;

	struct wlr_box current_box;
	struct wlr_box saved_floating_box;

	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
	struct wl_listener request_move;
	struct wl_listener request_resize;
	struct wl_listener request_maximize;
	struct wl_listener request_fullscreen;
};

struct termuxwc_workspace {
	int id; // 1 to NUM_WORKSPACES
	struct wl_list toplevels; // list of termuxwc_toplevel
	struct termuxwc_toplevel *focused;
};

struct termuxwc_popup {
	struct wlr_xdg_popup *xdg_popup;
	struct wlr_scene_tree *scene_tree;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct termuxwc_layer_surface {
	struct wl_list link;
	struct termuxwc_server *server;
	struct wlr_layer_surface_v1 *layer_surface;
	struct wlr_scene_layer_surface_v1 *scene_layer;
	struct wl_listener map;
	struct wl_listener unmap;
	struct wl_listener commit;
	struct wl_listener destroy;
};

struct termuxwc_output {
	struct wl_list link;
	struct termuxwc_server *server;
	struct wlr_output *wlr_output;
	struct wlr_box usable_area;
	struct wl_listener frame;
	struct wl_listener request_state;
	struct wl_listener destroy;
	struct wl_event_source *frame_timer;
};

struct termuxwc_keyboard {
	struct wl_list link;
	struct termuxwc_server *server;
	struct wlr_keyboard *wlr_keyboard;
	struct wl_listener modifiers;
	struct wl_listener key;
	struct wl_listener destroy;
};

/* Wallpaper scene node (opaque here; implemented in src/background.c). */
struct twc_wallpaper;

struct termuxwc_server {
	struct wl_display *wl_display;
	struct wl_event_loop *event_loop;
	struct wlr_backend *backend;
	struct wlr_renderer *renderer;
	struct wlr_allocator *allocator;

	struct wlr_scene *scene;
	struct wlr_scene_output_layout *scene_layout;
	struct wlr_scene_tree *scene_background;
	struct wlr_scene_tree *scene_bottom;
	struct wlr_scene_tree *scene_windows;
	struct wlr_scene_tree *scene_top;
	struct wlr_scene_tree *scene_overlay;
	struct wlr_scene_rect *background_rect;
	struct twc_wallpaper *background; // image wallpaper node (may be NULL)

	struct wlr_output_layout *output_layout;
	struct wl_list outputs;
	struct wl_listener new_output;

	struct wlr_xdg_shell *xdg_shell;
	struct wl_listener new_xdg_toplevel;
	struct wl_listener new_xdg_popup;

	struct wlr_layer_shell_v1 *layer_shell;
	struct wl_listener new_layer_surface;
	struct wl_list layer_surfaces;

	struct wlr_cursor *cursor;
	struct wlr_xcursor_manager *cursor_mgr;
	struct wl_listener cursor_motion;
	struct wl_listener cursor_motion_absolute;
	struct wl_listener cursor_button;
	struct wl_listener cursor_axis;
	struct wl_listener cursor_frame;

	struct wlr_seat *seat;
	struct wl_listener new_input;
	struct wl_listener request_cursor;
	struct wl_listener request_set_selection;
	struct wl_list keyboards;

	struct wlr_virtual_keyboard_manager_v1 *virtual_keyboard_mgr;
	struct wl_listener new_virtual_keyboard;
	struct wlr_virtual_pointer_manager_v1 *virtual_pointer_mgr;
	struct wl_listener new_virtual_pointer;

	enum termuxwc_cursor_mode cursor_mode;
	struct termuxwc_toplevel *grabbed_toplevel;
	double grab_x, grab_y;
	struct wlr_box grab_geobox;
	uint32_t resize_edges;

	// Workspaces (SwayWM style 1..9)
	int active_workspace;
	struct termuxwc_workspace workspaces[NUM_WORKSPACES];

	// Configuration
	struct termuxwc_config config;

	int default_width;
	int default_height;
	int default_refresh_rate;
};

/* --- Cross-module API (implemented across src/*.c) --- */

/* layout.c */
void twc_arrange_workspace(struct termuxwc_server *server, int ws_id);
void twc_arrange_all_workspaces(struct termuxwc_server *server);
void twc_update_window_borders(struct termuxwc_toplevel *toplevel, bool focused);
void twc_create_window_borders(struct termuxwc_toplevel *toplevel);

/* cursor.c */
struct termuxwc_toplevel *twc_desktop_toplevel_at(
		struct termuxwc_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy);
void twc_reset_cursor_mode(struct termuxwc_server *server);
void twc_begin_interactive(struct termuxwc_toplevel *toplevel,
		enum termuxwc_cursor_mode mode, uint32_t edges);
void twc_setup_cursor_listeners(struct termuxwc_server *server);

/* xdg.c */
void twc_focus_toplevel(struct termuxwc_toplevel *toplevel);
void twc_switch_workspace(struct termuxwc_server *server, int target_ws);
void twc_move_toplevel_to_workspace(struct termuxwc_toplevel *toplevel, int target_ws);
void twc_toggle_floating(struct termuxwc_toplevel *toplevel);
void twc_toggle_fullscreen(struct termuxwc_toplevel *toplevel);
void twc_close_active_window(struct termuxwc_server *server);
void twc_focus_next_window(struct termuxwc_server *server, bool reverse);
void twc_swap_next_window(struct termuxwc_server *server, bool reverse);

/* inputs.c */
void twc_setup_input(struct termuxwc_server *server);

/* layers.c */
void twc_arrange_layers(struct termuxwc_server *server);
void twc_setup_layer_shell(struct termuxwc_server *server);

/* xdg.c */
void twc_setup_xdg_shell(struct termuxwc_server *server);

/* termux_api.c */
const char *twc_termux_api_resolve_display(const char *cli_display);
struct wlr_backend *twc_termux_api_backend_create(
		struct wl_event_loop *event_loop, const char *cli_display);
void twc_termux_api_add_output(struct termuxwc_server *server);

/* outputs.c */
void twc_setup_outputs(struct termuxwc_server *server);
int twc_output_frame_interval_ms(const struct termuxwc_server *server);

#endif
