/*
 * termuxwc: A high-performance, SwayWM-grade tiling Wayland compositor
 * built from scratch for Termux (Android rootless) and Linux with wlroots-0.19.
 *
 * Features:
 * - Dynamic i3/Sway auto-tiling (landscape/portrait adaptive)
 * - 9 Virtual Workspaces with instant switching
 * - Active & inactive window borders with customizable colors & gaps
 * - Full Sway-compatible keybindings (Super/Alt mod support)
 * - Floating toggle, fullscreen toggle, window close, move & swap
 * - Full wayvnc streaming (screencopy, virtual pointer, virtual keyboard)
 * - Layer-Shell v1 support (waybar, wpaperd, swaybg)
 * - Human-friendly config file (~/.config/termuxwc/config)
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"

#include <assert.h>
#include <ctype.h>
#include <errno.h>
#include <linux/input-event-codes.h>
#include <getopt.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/backend/headless.h>
#include <wlr/render/allocator.h>
#include <wlr/render/pixman.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/types/wlr_linux_dmabuf_v1.h>
#include <wlr/types/wlr_compositor.h>
#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_data_device.h>
#include <wlr/types/wlr_fractional_scale_v1.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_presentation_time.h>
#include <wlr/types/wlr_primary_selection_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_screencopy_v1.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_subcompositor.h>
#include <wlr/types/wlr_text_input_v3.h>
#include <wlr/types/wlr_viewporter.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/types/wlr_xcursor_manager.h>
#include <wlr/types/wlr_xdg_activation_v1.h>
#include <wlr/types/wlr_xdg_decoration_v1.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

// Forward declarations
static void arrange_workspace(struct termuxwc_server *server, int ws_id);
static void arrange_all_workspaces(struct termuxwc_server *server);
static void arrange_layers(struct termuxwc_server *server);
static void focus_toplevel(struct termuxwc_toplevel *toplevel);
static void update_window_borders(struct termuxwc_toplevel *toplevel, bool focused);
static void switch_workspace(struct termuxwc_server *server, int target_ws);
static void move_toplevel_to_workspace(struct termuxwc_toplevel *toplevel, int target_ws);
static void toggle_floating(struct termuxwc_toplevel *toplevel);
static void toggle_fullscreen(struct termuxwc_toplevel *toplevel);
static void close_active_window(struct termuxwc_server *server);
static void focus_next_window(struct termuxwc_server *server, bool reverse);
static void swap_next_window(struct termuxwc_server *server, bool reverse);

/* --- Configuration System --- */

static void parse_hex_color(const char *hex, float color[4]) {
	if (hex[0] == '#') hex++;
	unsigned int r = 0, g = 0, b = 0;
	if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) {
		color[0] = r / 255.0f;
		color[1] = g / 255.0f;
		color[2] = b / 255.0f;
		color[3] = 1.0f;
	}
}

static void load_or_create_config(struct termuxwc_config *cfg) {
	// Sane defaults
	cfg->mod_mask = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT; // Support both Super & Alt!
	cfg->inner_gap = 6;
	cfg->outer_gap = 8;
	cfg->border_width = 2;
	snprintf(cfg->terminal_cmd, sizeof(cfg->terminal_cmd), "foot");
	snprintf(cfg->launcher_cmd, sizeof(cfg->launcher_cmd), "foot -e fzf");
	parse_hex_color("#7aa2f7", cfg->color_active_border);   // TokyoNight blue
	parse_hex_color("#3b4261", cfg->color_inactive_border); // Subtle slate
	parse_hex_color("#16161e", cfg->color_background);      // Deep dark background

	const char *home = getenv("HOME");
	if (!home) return;

	char config_dir[512], config_path[512];
	snprintf(config_dir, sizeof(config_dir), "%s/.config/termuxwc", home);
	snprintf(config_path, sizeof(config_path), "%s/config", config_dir);

	mkdir(config_dir, 0755);

	FILE *f = fopen(config_path, "r");
	if (!f) {
		// Generate clean documented default config
		f = fopen(config_path, "w");
		if (f) {
			fprintf(f, "# termuxwc Sway-grade Tiling Window Manager Configuration\n");
			fprintf(f, "# Mod key: Mod4 = Super/Windows key, Mod1 = Alt key, ModBoth = both\n");
			fprintf(f, "mod ModBoth\n\n");
			fprintf(f, "# Application commands\n");
			fprintf(f, "terminal foot\n");
			fprintf(f, "launcher foot -e fzf\n\n");
			fprintf(f, "# Geometry & Gaps (pixels)\n");
			fprintf(f, "inner_gap 6\n");
			fprintf(f, "outer_gap 8\n");
			fprintf(f, "border_width 2\n\n");
			fprintf(f, "# Colors (Hex)\n");
			fprintf(f, "active_border #7aa2f7\n");
			fprintf(f, "inactive_border #3b4261\n");
			fprintf(f, "background #16161e\n");
			fclose(f);
		}
		return;
	}

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (*p == '#' || *p == '\0') continue;

		char key[64], val[192];
		if (sscanf(p, "%63s %191[^\n]", key, val) == 2) {
			if (strcmp(key, "terminal") == 0) {
				snprintf(cfg->terminal_cmd, sizeof(cfg->terminal_cmd), "%s", val);
			} else if (strcmp(key, "launcher") == 0) {
				snprintf(cfg->launcher_cmd, sizeof(cfg->launcher_cmd), "%s", val);
			} else if (strcmp(key, "inner_gap") == 0) {
				cfg->inner_gap = atoi(val);
			} else if (strcmp(key, "outer_gap") == 0) {
				cfg->outer_gap = atoi(val);
			} else if (strcmp(key, "border_width") == 0) {
				cfg->border_width = atoi(val);
			} else if (strcmp(key, "active_border") == 0) {
				parse_hex_color(val, cfg->color_active_border);
			} else if (strcmp(key, "inactive_border") == 0) {
				parse_hex_color(val, cfg->color_inactive_border);
			} else if (strcmp(key, "background") == 0) {
				parse_hex_color(val, cfg->color_background);
			} else if (strcmp(key, "mod") == 0) {
				if (strcmp(val, "Mod4") == 0) cfg->mod_mask = WLR_MODIFIER_LOGO;
				else if (strcmp(val, "Mod1") == 0) cfg->mod_mask = WLR_MODIFIER_ALT;
				else cfg->mod_mask = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT;
			}
		}
	}
	fclose(f);
}

static pid_t launch_command(const char *cmd, const char *socket) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed for command");
		return -1;
	}
	if (pid == 0) {
		setenv("WAYLAND_DISPLAY", socket, 1);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	return pid;
}

/* --- Window Borders & Decorations --- */

static void create_window_borders(struct termuxwc_toplevel *toplevel) {
	float *color = toplevel->server->config.color_inactive_border;
	toplevel->border_top = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
	toplevel->border_bottom = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
	toplevel->border_left = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
	toplevel->border_right = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
}

static void update_window_borders(struct termuxwc_toplevel *toplevel, bool focused) {
	int bw = toplevel->server->config.border_width;
	if (toplevel->is_fullscreen || bw <= 0) {
		wlr_scene_node_set_enabled(&toplevel->border_top->node, false);
		wlr_scene_node_set_enabled(&toplevel->border_bottom->node, false);
		wlr_scene_node_set_enabled(&toplevel->border_left->node, false);
		wlr_scene_node_set_enabled(&toplevel->border_right->node, false);
		return;
	}

	wlr_scene_node_set_enabled(&toplevel->border_top->node, true);
	wlr_scene_node_set_enabled(&toplevel->border_bottom->node, true);
	wlr_scene_node_set_enabled(&toplevel->border_left->node, true);
	wlr_scene_node_set_enabled(&toplevel->border_right->node, true);

	float *color = focused ? toplevel->server->config.color_active_border
	                       : toplevel->server->config.color_inactive_border;

	wlr_scene_rect_set_color(toplevel->border_top, color);
	wlr_scene_rect_set_color(toplevel->border_bottom, color);
	wlr_scene_rect_set_color(toplevel->border_left, color);
	wlr_scene_rect_set_color(toplevel->border_right, color);

	int w = toplevel->current_box.width;
	int h = toplevel->current_box.height;

	// Position 4 border rects relative to toplevel->scene_tree origin (0, 0)
	wlr_scene_node_set_position(&toplevel->border_top->node, 0, 0);
	wlr_scene_rect_set_size(toplevel->border_top, w, bw);

	wlr_scene_node_set_position(&toplevel->border_bottom->node, 0, h - bw);
	wlr_scene_rect_set_size(toplevel->border_bottom, w, bw);

	wlr_scene_node_set_position(&toplevel->border_left->node, 0, 0);
	wlr_scene_rect_set_size(toplevel->border_left, bw, h);

	wlr_scene_node_set_position(&toplevel->border_right->node, w - bw, 0);
	wlr_scene_rect_set_size(toplevel->border_right, bw, h);
}

/* --- Tiling Engine --- */

static struct termuxwc_output *get_primary_output(struct termuxwc_server *server) {
	if (wl_list_empty(&server->outputs)) return NULL;
	struct termuxwc_output *out = NULL;
	return wl_container_of(server->outputs.next, out, link);
}

static void arrange_workspace(struct termuxwc_server *server, int ws_id) {
	if (ws_id < 1 || ws_id > NUM_WORKSPACES) return;
	struct termuxwc_workspace *ws = &server->workspaces[ws_id - 1];

	struct termuxwc_output *output = get_primary_output(server);
	if (!output) return;

	struct wlr_box usable = output->usable_area;
	if (usable.width <= 0 || usable.height <= 0) {
		usable.x = 0;
		usable.y = 0;
		usable.width = output->wlr_output->width;
		usable.height = output->wlr_output->height;
	}

	// Check if any window is fullscreen
	struct termuxwc_toplevel *toplevel;
	wl_list_for_each(toplevel, &ws->toplevels, link) {
		if (toplevel->is_fullscreen) {
			toplevel->current_box = usable;
			wlr_scene_node_set_position(&toplevel->scene_tree->node, usable.x, usable.y);
			wlr_scene_node_set_position(
				&((struct wlr_scene_tree *)toplevel->xdg_toplevel->base->data)->node, 0, 0);
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, usable.width, usable.height);
			update_window_borders(toplevel, toplevel == ws->focused);
			return; // Fullscreen window takes over entire active display
		}
	}

	// Collect tiled windows
	int num_tiled = 0;
	struct termuxwc_toplevel *tiled[128];
	wl_list_for_each(toplevel, &ws->toplevels, link) {
		if (!toplevel->is_floating) {
			if (num_tiled < 128) tiled[num_tiled++] = toplevel;
		} else {
			// Floating window: position at saved geometry
			int bw = server->config.border_width;
			wlr_scene_node_set_position(&toplevel->scene_tree->node,
				toplevel->current_box.x, toplevel->current_box.y);
			wlr_scene_node_set_position(
				&((struct wlr_scene_tree *)toplevel->xdg_toplevel->base->data)->node, bw, bw);
			int cw = toplevel->current_box.width - 2 * bw;
			int ch = toplevel->current_box.height - 2 * bw;
			if (cw < 1) cw = 1;
			if (ch < 1) ch = 1;
			wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, cw, ch);
			update_window_borders(toplevel, toplevel == ws->focused);
		}
	}

	if (num_tiled == 0) return;

	int ig = server->config.inner_gap;
	int og = server->config.outer_gap;
	int bw = server->config.border_width;

	int ax = usable.x + og;
	int ay = usable.y + og;
	int aw = usable.width - 2 * og;
	int ah = usable.height - 2 * og;
	if (aw <= 0 || ah <= 0) return;

	if (num_tiled == 1) {
		struct termuxwc_toplevel *w = tiled[0];
		w->current_box = (struct wlr_box){ ax, ay, aw, ah };
		wlr_scene_node_set_position(&w->scene_tree->node, ax, ay);
		wlr_scene_node_set_position(
			&((struct wlr_scene_tree *)w->xdg_toplevel->base->data)->node, bw, bw);
		wlr_xdg_toplevel_set_size(w->xdg_toplevel, aw - 2 * bw, ah - 2 * bw);
		update_window_borders(w, w == ws->focused);
	} else if (num_tiled == 2) {
		bool landscape = aw >= ah;
		if (landscape) {
			int w = (aw - ig) / 2;
			for (int i = 0; i < 2; i++) {
				int x = ax + i * (w + ig);
				int cur_w = (i == 1) ? (aw - w - ig) : w;
				tiled[i]->current_box = (struct wlr_box){ x, ay, cur_w, ah };
				wlr_scene_node_set_position(&tiled[i]->scene_tree->node, x, ay);
				wlr_scene_node_set_position(
					&((struct wlr_scene_tree *)tiled[i]->xdg_toplevel->base->data)->node, bw, bw);
				wlr_xdg_toplevel_set_size(tiled[i]->xdg_toplevel, cur_w - 2 * bw, ah - 2 * bw);
				update_window_borders(tiled[i], tiled[i] == ws->focused);
			}
		} else {
			// Portrait mode (phone screen)
			int h = (ah - ig) / 2;
			for (int i = 0; i < 2; i++) {
				int y = ay + i * (h + ig);
				int cur_h = (i == 1) ? (ah - h - ig) : h;
				tiled[i]->current_box = (struct wlr_box){ ax, y, aw, cur_h };
				wlr_scene_node_set_position(&tiled[i]->scene_tree->node, ax, y);
				wlr_scene_node_set_position(
					&((struct wlr_scene_tree *)tiled[i]->xdg_toplevel->base->data)->node, bw, bw);
				wlr_xdg_toplevel_set_size(tiled[i]->xdg_toplevel, aw - 2 * bw, cur_h - 2 * bw);
				update_window_borders(tiled[i], tiled[i] == ws->focused);
			}
		}
	} else {
		// Master-Stack Layout (Landscape or Portrait adaptive)
		bool landscape = aw >= ah;
		int stack_count = num_tiled - 1;

		if (landscape) {
			int mw = (aw - ig) / 2;
			int sw = aw - mw - ig;

			// Master window (left)
			tiled[0]->current_box = (struct wlr_box){ ax, ay, mw, ah };
			wlr_scene_node_set_position(&tiled[0]->scene_tree->node, ax, ay);
			wlr_scene_node_set_position(
				&((struct wlr_scene_tree *)tiled[0]->xdg_toplevel->base->data)->node, bw, bw);
			wlr_xdg_toplevel_set_size(tiled[0]->xdg_toplevel, mw - 2 * bw, ah - 2 * bw);
			update_window_borders(tiled[0], tiled[0] == ws->focused);

			// Stack column (right)
			int total_stack_gaps = (stack_count - 1) * ig;
			int sh = (ah - total_stack_gaps) / stack_count;
			int sx = ax + mw + ig;

			for (int i = 1; i < num_tiled; i++) {
				int sy = ay + (i - 1) * (sh + ig);
				int cur_sh = (i == num_tiled - 1) ? (ah - (sy - ay)) : sh;
				tiled[i]->current_box = (struct wlr_box){ sx, sy, sw, cur_sh };
				wlr_scene_node_set_position(&tiled[i]->scene_tree->node, sx, sy);
				wlr_scene_node_set_position(
					&((struct wlr_scene_tree *)tiled[i]->xdg_toplevel->base->data)->node, bw, bw);
				wlr_xdg_toplevel_set_size(tiled[i]->xdg_toplevel, sw - 2 * bw, cur_sh - 2 * bw);
				update_window_borders(tiled[i], tiled[i] == ws->focused);
			}
		} else {
			// Portrait mode: Master on top, stack below
			int mh = (ah - ig) / 2;
			int sh = ah - mh - ig;

			// Master window (top)
			tiled[0]->current_box = (struct wlr_box){ ax, ay, aw, mh };
			wlr_scene_node_set_position(&tiled[0]->scene_tree->node, ax, ay);
			wlr_scene_node_set_position(
				&((struct wlr_scene_tree *)tiled[0]->xdg_toplevel->base->data)->node, bw, bw);
			wlr_xdg_toplevel_set_size(tiled[0]->xdg_toplevel, aw - 2 * bw, mh - 2 * bw);
			update_window_borders(tiled[0], tiled[0] == ws->focused);

			// Stack row (bottom)
			int total_stack_gaps = (stack_count - 1) * ig;
			int sw = (aw - total_stack_gaps) / stack_count;
			int sy = ay + mh + ig;

			for (int i = 1; i < num_tiled; i++) {
				int sx = ax + (i - 1) * (sw + ig);
				int cur_sw = (i == num_tiled - 1) ? (aw - (sx - ax)) : sw;
				tiled[i]->current_box = (struct wlr_box){ sx, sy, cur_sw, sh };
				wlr_scene_node_set_position(&tiled[i]->scene_tree->node, sx, sy);
				wlr_scene_node_set_position(
					&((struct wlr_scene_tree *)tiled[i]->xdg_toplevel->base->data)->node, bw, bw);
				wlr_xdg_toplevel_set_size(tiled[i]->xdg_toplevel, cur_sw - 2 * bw, sh - 2 * bw);
				update_window_borders(tiled[i], tiled[i] == ws->focused);
			}
		}
	}
}

static void arrange_all_workspaces(struct termuxwc_server *server) {
	for (int i = 1; i <= NUM_WORKSPACES; i++) {
		arrange_workspace(server, i);
	}
}

/* --- Workspace & Focus Actions --- */

static void focus_toplevel(struct termuxwc_toplevel *toplevel) {
	if (!toplevel) return;
	struct termuxwc_server *server = toplevel->server;
	struct termuxwc_workspace *ws = &server->workspaces[toplevel->workspace_id - 1];
	struct wlr_seat *seat = server->seat;

	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

	if (ws->focused && ws->focused != toplevel) {
		update_window_borders(ws->focused, false);
	}
	ws->focused = toplevel;

	if (prev_surface != NULL && prev_surface != surface) {
		struct wlr_xdg_toplevel *prev_top =
			wlr_xdg_toplevel_try_from_wlr_surface(prev_surface);
		if (prev_top != NULL) {
			wlr_xdg_toplevel_set_activated(prev_top, false);
		}
	}

	wlr_scene_node_raise_to_top(&toplevel->scene_tree->node);
	wlr_xdg_toplevel_set_activated(toplevel->xdg_toplevel, true);
	update_window_borders(toplevel, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

static void switch_workspace(struct termuxwc_server *server, int target_ws) {
	if (target_ws < 1 || target_ws > NUM_WORKSPACES) return;
	if (target_ws == server->active_workspace) return;

	// Hide windows on old workspace, show windows on new workspace
	for (int i = 1; i <= NUM_WORKSPACES; i++) {
		struct termuxwc_workspace *ws = &server->workspaces[i - 1];
		bool enable = (i == target_ws);
		struct termuxwc_toplevel *top;
		wl_list_for_each(top, &ws->toplevels, link) {
			wlr_scene_node_set_enabled(&top->scene_tree->node, enable);
		}
	}

	server->active_workspace = target_ws;
	arrange_workspace(server, target_ws);

	struct termuxwc_workspace *cur = &server->workspaces[target_ws - 1];
	if (cur->focused) {
		focus_toplevel(cur->focused);
	} else if (!wl_list_empty(&cur->toplevels)) {
		struct termuxwc_toplevel *first =
			wl_container_of(cur->toplevels.next, first, link);
		focus_toplevel(first);
	} else {
		wlr_seat_keyboard_clear_focus(server->seat);
	}

	wlr_log(WLR_INFO, "Switched to Workspace %d", target_ws);
}

static void move_toplevel_to_workspace(struct termuxwc_toplevel *toplevel, int target_ws) {
	if (!toplevel || target_ws < 1 || target_ws > NUM_WORKSPACES) return;
	if (toplevel->workspace_id == target_ws) return;

	int old_ws = toplevel->workspace_id;
	struct termuxwc_server *server = toplevel->server;

	wl_list_remove(&toplevel->link);
	wl_list_insert(&server->workspaces[target_ws - 1].toplevels, &toplevel->link);
	toplevel->workspace_id = target_ws;

	if (target_ws != server->active_workspace) {
		wlr_scene_node_set_enabled(&toplevel->scene_tree->node, false);
	}

	arrange_workspace(server, old_ws);
	arrange_workspace(server, target_ws);

	struct termuxwc_workspace *cur_ws = &server->workspaces[server->active_workspace - 1];
	if (!wl_list_empty(&cur_ws->toplevels)) {
		struct termuxwc_toplevel *next_top =
			wl_container_of(cur_ws->toplevels.next, next_top, link);
		focus_toplevel(next_top);
	} else {
		cur_ws->focused = NULL;
		wlr_seat_keyboard_clear_focus(server->seat);
	}

	wlr_log(WLR_INFO, "Moved window to Workspace %d", target_ws);
}

static void toggle_floating(struct termuxwc_toplevel *toplevel) {
	if (!toplevel || toplevel->is_fullscreen) return;
	toplevel->is_floating = !toplevel->is_floating;
	if (toplevel->is_floating) {
		if (toplevel->saved_floating_box.width <= 0) {
			toplevel->current_box = (struct wlr_box){ 100, 100, 800, 500 };
		} else {
			toplevel->current_box = toplevel->saved_floating_box;
		}
	} else {
		toplevel->saved_floating_box = toplevel->current_box;
	}
	arrange_workspace(toplevel->server, toplevel->workspace_id);
}

static void toggle_fullscreen(struct termuxwc_toplevel *toplevel) {
	if (!toplevel) return;
	toplevel->is_fullscreen = !toplevel->is_fullscreen;
	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, toplevel->is_fullscreen);
	arrange_workspace(toplevel->server, toplevel->workspace_id);
}

static void close_active_window(struct termuxwc_server *server) {
	struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
	if (ws->focused && ws->focused->xdg_toplevel) {
		wlr_xdg_toplevel_send_close(ws->focused->xdg_toplevel);
	}
}

static void focus_next_window(struct termuxwc_server *server, bool reverse) {
	struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
	if (wl_list_empty(&ws->toplevels)) return;

	struct termuxwc_toplevel *cur = ws->focused;
	struct wl_list *target_link = NULL;

	if (!cur) {
		target_link = ws->toplevels.next;
	} else {
		target_link = reverse ? cur->link.prev : cur->link.next;
		if (target_link == &ws->toplevels) {
			target_link = reverse ? ws->toplevels.prev : ws->toplevels.next;
		}
	}

	struct termuxwc_toplevel *target = wl_container_of(target_link, target, link);
	focus_toplevel(target);
}

static void swap_next_window(struct termuxwc_server *server, bool reverse) {
	struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
	if (wl_list_length(&ws->toplevels) < 2) return;

	struct termuxwc_toplevel *cur = ws->focused;
	if (!cur) return;

	struct wl_list *target_link = reverse ? cur->link.prev : cur->link.next;
	if (target_link == &ws->toplevels) {
		target_link = reverse ? ws->toplevels.prev : ws->toplevels.next;
	}

	struct termuxwc_toplevel *neighbor = wl_container_of(target_link, neighbor, link);

	// Swap nodes in circular linked list
	wl_list_remove(&cur->link);
	if (reverse) {
		wl_list_insert(neighbor->link.prev, &cur->link);
	} else {
		wl_list_insert(&neighbor->link, &cur->link);
	}

	arrange_workspace(server, server->active_workspace);
	focus_toplevel(cur);
}

/* --- Window Query & Interactivity --- */

static struct termuxwc_toplevel *desktop_toplevel_at(
		struct termuxwc_server *server, double lx, double ly,
		struct wlr_surface **surface, double *sx, double *sy) {
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene_windows->node, lx, ly, sx, sy);
	if (!node || node->type != WLR_SCENE_NODE_BUFFER) return NULL;

	struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
	struct wlr_scene_surface *scene_surface =
		wlr_scene_surface_try_from_buffer(scene_buffer);
	if (!scene_surface) return NULL;

	*surface = scene_surface->surface;
	struct wlr_scene_tree *tree = node->parent;
	while (tree != NULL && tree->node.data == NULL) {
		tree = tree->node.parent;
	}
	return tree ? tree->node.data : NULL;
}

static void reset_cursor_mode(struct termuxwc_server *server) {
	server->cursor_mode = TERMUXWC_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

static void begin_interactive(struct termuxwc_toplevel *toplevel,
		enum termuxwc_cursor_mode mode, uint32_t edges) {
	struct termuxwc_server *server = toplevel->server;

	// Floating conversion on interactive drag/resize
	if (!toplevel->is_floating) {
		toggle_floating(toplevel);
	}

	server->grabbed_toplevel = toplevel;
	server->cursor_mode = mode;

	if (mode == TERMUXWC_CURSOR_MOVE) {
		server->grab_x = server->cursor->x - toplevel->scene_tree->node.x;
		server->grab_y = server->cursor->y - toplevel->scene_tree->node.y;
	} else {
		double border_x = toplevel->scene_tree->node.x +
			((edges & WLR_EDGE_RIGHT) ? toplevel->current_box.width : 0);
		double border_y = toplevel->scene_tree->node.y +
			((edges & WLR_EDGE_BOTTOM) ? toplevel->current_box.height : 0);
		server->grab_x = server->cursor->x - border_x;
		server->grab_y = server->cursor->y - border_y;

		server->grab_geobox = toplevel->current_box;
		server->resize_edges = edges;
	}
}

static void process_cursor_move(struct termuxwc_server *server) {
	struct termuxwc_toplevel *toplevel = server->grabbed_toplevel;
	if (!toplevel) return;
	int nx = server->cursor->x - server->grab_x;
	int ny = server->cursor->y - server->grab_y;
	toplevel->current_box.x = nx;
	toplevel->current_box.y = ny;
	wlr_scene_node_set_position(&toplevel->scene_tree->node, nx, ny);
}

static void process_cursor_resize(struct termuxwc_server *server) {
	struct termuxwc_toplevel *toplevel = server->grabbed_toplevel;
	if (!toplevel) return;

	double border_x = server->cursor->x - server->grab_x;
	double border_y = server->cursor->y - server->grab_y;
	int new_left = server->grab_geobox.x;
	int new_right = server->grab_geobox.x + server->grab_geobox.width;
	int new_top = server->grab_geobox.y;
	int new_bottom = server->grab_geobox.y + server->grab_geobox.height;

	if (server->resize_edges & WLR_EDGE_TOP) {
		new_top = border_y;
		if (new_top >= new_bottom) new_top = new_bottom - 1;
	} else if (server->resize_edges & WLR_EDGE_BOTTOM) {
		new_bottom = border_y;
		if (new_bottom <= new_top) new_bottom = new_top + 1;
	}

	if (server->resize_edges & WLR_EDGE_LEFT) {
		new_left = border_x;
		if (new_left >= new_right) new_left = new_right - 1;
	} else if (server->resize_edges & WLR_EDGE_RIGHT) {
		new_right = border_x;
		if (new_right <= new_left) new_right = new_left + 1;
	}

	int nw = new_right - new_left;
	int nh = new_bottom - new_top;
	int bw = server->config.border_width;

	toplevel->current_box = (struct wlr_box){ new_left, new_top, nw, nh };
	wlr_scene_node_set_position(&toplevel->scene_tree->node, new_left, new_top);
	wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, nw - 2 * bw, nh - 2 * bw);
	update_window_borders(toplevel, true);
}

static void process_cursor_motion(struct termuxwc_server *server, uint32_t time) {
	if (server->cursor_mode == TERMUXWC_CURSOR_MOVE) {
		process_cursor_move(server);
		return;
	} else if (server->cursor_mode == TERMUXWC_CURSOR_RESIZE) {
		process_cursor_resize(server);
		return;
	}

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct wlr_scene_node *node = wlr_scene_node_at(
		&server->scene->tree.node, server->cursor->x, server->cursor->y, &sx, &sy);

	if (node && node->type == WLR_SCENE_NODE_BUFFER) {
		struct wlr_scene_buffer *scene_buffer = wlr_scene_buffer_from_node(node);
		struct wlr_scene_surface *scene_surface =
			wlr_scene_surface_try_from_buffer(scene_buffer);
		if (scene_surface) {
			surface = scene_surface->surface;
		}
	}

	if (surface) {
		wlr_seat_pointer_notify_enter(server->seat, surface, sx, sy);
		wlr_seat_pointer_notify_motion(server->seat, time, sx, sy);
	} else {
		wlr_seat_pointer_clear_focus(server->seat);
	}
}

static void server_cursor_motion(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, cursor_motion);
	struct wlr_pointer_motion_event *event = data;
	wlr_cursor_move(server->cursor, &event->pointer->base,
		event->delta_x, event->delta_y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_motion_absolute(
		struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, cursor_motion_absolute);
	struct wlr_pointer_motion_absolute_event *event = data;
	wlr_cursor_warp_absolute(server->cursor, &event->pointer->base,
		event->x, event->y);
	process_cursor_motion(server, event->time_msec);
}

static void server_cursor_button(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, cursor_button);
	struct wlr_pointer_button_event *event = data;
	wlr_seat_pointer_notify_button(server->seat,
		event->time_msec, event->button, event->state);

	double sx, sy;
	struct wlr_surface *surface = NULL;
	struct termuxwc_toplevel *toplevel = desktop_toplevel_at(
		server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->cursor_mode != TERMUXWC_CURSOR_PASSTHROUGH) {
			reset_cursor_mode(server);
		}
	} else {
		if (toplevel != NULL) {
			focus_toplevel(toplevel);

			// Check Mod+Click for interactive dragging / resizing
			struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
			uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
			bool mod_pressed = (mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT));
			if (mod_pressed) {
				if (event->button == BTN_LEFT) {
					begin_interactive(toplevel, TERMUXWC_CURSOR_MOVE, 0);
				} else if (event->button == BTN_RIGHT) {
					begin_interactive(toplevel, TERMUXWC_CURSOR_RESIZE,
						WLR_EDGE_RIGHT | WLR_EDGE_BOTTOM);
				}
			}
		}
	}
}

static void server_cursor_axis(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, cursor_axis);
	struct wlr_pointer_axis_event *event = data;
	wlr_seat_pointer_notify_axis(server->seat,
		event->time_msec, event->orientation, event->delta,
		event->delta_discrete, event->source, event->relative_direction);
}

static void server_cursor_frame(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, cursor_frame);
	(void)data;
	wlr_seat_pointer_notify_frame(server->seat);
}

static void seat_request_cursor(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, request_cursor);
	struct wlr_seat_pointer_request_set_cursor_event *event = data;
	struct wlr_seat_client *focused_client =
		server->seat->pointer_state.focused_client;
	if (focused_client == event->seat_client) {
		wlr_cursor_set_surface(server->cursor, event->surface,
			event->hotspot_x, event->hotspot_y);
	}
}

static void seat_request_set_selection(
		struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, request_set_selection);
	struct wlr_seat_request_set_selection_event *event = data;
	wlr_seat_set_selection(server->seat, event->source, event->serial);
}

/* --- Keyboard Handling & Sway Keybindings --- */

static void keyboard_handle_modifiers(
		struct wl_listener *listener, void *data) {
	struct termuxwc_keyboard *keyboard =
		wl_container_of(listener, keyboard, modifiers);
	(void)data;
	wlr_seat_set_keyboard(keyboard->server->seat, keyboard->wlr_keyboard);
	wlr_seat_keyboard_notify_modifiers(keyboard->server->seat,
		&keyboard->wlr_keyboard->modifiers);
}

static void keyboard_handle_key(struct wl_listener *listener, void *data) {
	struct termuxwc_keyboard *keyboard =
		wl_container_of(listener, keyboard, key);
	struct termuxwc_server *server = keyboard->server;
	struct wlr_keyboard_key_event *event = data;
	struct wlr_seat *seat = server->seat;

	uint32_t keycode = event->keycode + 8;
	const xkb_keysym_t *syms;
	int nsyms = xkb_state_key_get_syms(
		keyboard->wlr_keyboard->xkb_state, keycode, &syms);

	bool handled = false;
	uint32_t modifiers = wlr_keyboard_get_modifiers(keyboard->wlr_keyboard);
	bool mod_active = (modifiers & server->config.mod_mask) ||
	                  (modifiers & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT));
	bool shift = (modifiers & WLR_MODIFIER_SHIFT);

	if (event->state == WL_KEYBOARD_KEY_STATE_PRESSED && mod_active) {
		const char *socket = getenv("WAYLAND_DISPLAY");
		for (int i = 0; i < nsyms; i++) {
			xkb_keysym_t sym = syms[i];

			// Mod + Return: Launch Terminal
			if (sym == XKB_KEY_Return) {
				launch_command(server->config.terminal_cmd, socket);
				handled = true;
				break;
			}
			// Mod + D: Launch App Launcher
			else if (sym == XKB_KEY_d || sym == XKB_KEY_D) {
				launch_command(server->config.launcher_cmd, socket);
				handled = true;
				break;
			}
			// Mod + Q or Mod + Shift + C: Close Window
			else if (sym == XKB_KEY_q || sym == XKB_KEY_Q ||
			         (shift && (sym == XKB_KEY_c || sym == XKB_KEY_C))) {
				close_active_window(server);
				handled = true;
				break;
			}
			// Mod + F: Toggle Fullscreen
			else if (sym == XKB_KEY_f || sym == XKB_KEY_F) {
				struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
				if (ws->focused) toggle_fullscreen(ws->focused);
				handled = true;
				break;
			}
			// Mod + Space or Mod + Shift + Space: Toggle Floating
			else if (sym == XKB_KEY_space) {
				struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
				if (ws->focused) toggle_floating(ws->focused);
				handled = true;
				break;
			}
			// Mod + H/J/K/L or Arrows: Directional Focus / Swap
			else if (sym == XKB_KEY_j || sym == XKB_KEY_J || sym == XKB_KEY_Down ||
			         sym == XKB_KEY_l || sym == XKB_KEY_L || sym == XKB_KEY_Right) {
				if (shift) swap_next_window(server, false);
				else focus_next_window(server, false);
				handled = true;
				break;
			}
			else if (sym == XKB_KEY_k || sym == XKB_KEY_K || sym == XKB_KEY_Up ||
			         sym == XKB_KEY_h || sym == XKB_KEY_H || sym == XKB_KEY_Left) {
				if (shift) swap_next_window(server, true);
				else focus_next_window(server, true);
				handled = true;
				break;
			}
			// Mod + 1..9: Workspace Switching & Window Moving
			else if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
				int target_ws = sym - XKB_KEY_1 + 1;
				if (shift) {
					struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
					if (ws->focused) move_toplevel_to_workspace(ws->focused, target_ws);
				} else {
					switch_workspace(server, target_ws);
				}
				handled = true;
				break;
			}
			// Mod + Shift + E or Ctrl+Alt+Escape: Exit
			else if ((shift && (sym == XKB_KEY_e || sym == XKB_KEY_E)) ||
			         ((modifiers & WLR_MODIFIER_CTRL) && (sym == XKB_KEY_Escape))) {
				wlr_log(WLR_INFO, "Exit requested; shutting down compositor.");
				wl_display_terminate(server->wl_display);
				handled = true;
				break;
			}
		}
	}

	if (!handled) {
		wlr_seat_set_keyboard(seat, keyboard->wlr_keyboard);
		wlr_seat_keyboard_notify_key(seat,
			event->time_msec, event->keycode, event->state);
	}
}

static void keyboard_handle_destroy(struct wl_listener *listener, void *data) {
	struct termuxwc_keyboard *keyboard =
		wl_container_of(listener, keyboard, destroy);
	(void)data;
	wl_list_remove(&keyboard->modifiers.link);
	wl_list_remove(&keyboard->key.link);
	wl_list_remove(&keyboard->destroy.link);
	wl_list_remove(&keyboard->link);
	free(keyboard);
}

static void setup_keyboard_device(struct termuxwc_server *server, struct wlr_keyboard *wlr_keyboard) {
	struct termuxwc_keyboard *keyboard = calloc(1, sizeof(*keyboard));
	keyboard->server = server;
	keyboard->wlr_keyboard = wlr_keyboard;

	keyboard->modifiers.notify = keyboard_handle_modifiers;
	wl_signal_add(&wlr_keyboard->events.modifiers, &keyboard->modifiers);
	keyboard->key.notify = keyboard_handle_key;
	wl_signal_add(&wlr_keyboard->events.key, &keyboard->key);
	keyboard->destroy.notify = keyboard_handle_destroy;
	wl_signal_add(&wlr_keyboard->base.events.destroy, &keyboard->destroy);

	wlr_seat_set_keyboard(server->seat, wlr_keyboard);
	wl_list_insert(&server->keyboards, &keyboard->link);

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER | WL_SEAT_CAPABILITY_KEYBOARD;
	wlr_seat_set_capabilities(server->seat, caps);

	// Refocus active window with new keyboard
	struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
	if (ws->focused) {
		focus_toplevel(ws->focused);
	} else if (!wl_list_empty(&ws->toplevels)) {
		struct termuxwc_toplevel *top =
			wl_container_of(ws->toplevels.next, top, link);
		focus_toplevel(top);
	}
}

static void server_new_keyboard(
		struct termuxwc_server *server, struct wlr_input_device *device) {
	struct wlr_keyboard *wlr_keyboard = wlr_keyboard_from_input_device(device);

	struct xkb_context *context = xkb_context_new(XKB_CONTEXT_NO_FLAGS);
	struct xkb_keymap *keymap = xkb_keymap_new_from_names(
		context, NULL, XKB_KEYMAP_COMPILE_NO_FLAGS);
	wlr_keyboard_set_keymap(wlr_keyboard, keymap);
	xkb_keymap_unref(keymap);
	xkb_context_unref(context);
	wlr_keyboard_set_repeat_info(wlr_keyboard, 25, 600);

	setup_keyboard_device(server, wlr_keyboard);
}

static void server_new_pointer(
		struct termuxwc_server *server, struct wlr_input_device *device) {
	wlr_cursor_attach_input_device(server->cursor, device);
}

static void server_new_input(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_input);
	struct wlr_input_device *device = data;

	switch (device->type) {
	case WLR_INPUT_DEVICE_KEYBOARD:
		server_new_keyboard(server, device);
		break;
	case WLR_INPUT_DEVICE_POINTER:
		server_new_pointer(server, device);
		break;
	default:
		break;
	}

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

static void server_new_virtual_keyboard(
		struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	setup_keyboard_device(server, &virtual_keyboard->keyboard);
}

static void server_new_virtual_pointer(
		struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;

	wlr_cursor_attach_input_device(
		server->cursor, &event->new_pointer->pointer.base);

	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
}

/* --- Output & Rendering --- */

static void output_frame(struct wl_listener *listener, void *data) {
	struct termuxwc_output *output =
		wl_container_of(listener, output, frame);
	(void)data;
	struct wlr_scene *scene = output->server->scene;
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(scene, output->wlr_output);
	if (!scene_output) return;

	wlr_scene_output_commit(scene_output, NULL);

	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);
	wlr_scene_output_send_frame_done(scene_output, &now);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct termuxwc_output *output =
		wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);

	output->usable_area = (struct wlr_box){
		0, 0, output->wlr_output->width, output->wlr_output->height
	};

	arrange_layers(output->server);
	arrange_all_workspaces(output->server);
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct termuxwc_output *output =
		wl_container_of(listener, output, destroy);
	(void)data;
	wl_list_remove(&output->frame.link);
	wl_list_remove(&output->request_state.link);
	wl_list_remove(&output->destroy.link);
	wl_list_remove(&output->link);
	free(output);
}

static void server_new_output(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_output);
	struct wlr_output *wlr_output = data;

	wlr_output_init_render(wlr_output, server->allocator, server->renderer);

	struct wlr_output_state state;
	wlr_output_state_init(&state);
	wlr_output_state_set_enabled(&state, true);

	struct wlr_output_mode *mode = wlr_output_preferred_mode(wlr_output);
	if (mode != NULL) {
		wlr_output_state_set_mode(&state, mode);
	} else {
		wlr_output_state_set_custom_mode(&state,
			server->default_width, server->default_height, 60000);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	wlr_output_lock_software_cursors(wlr_output, true);

	struct termuxwc_output *output = calloc(1, sizeof(*output));
	output->wlr_output = wlr_output;
	output->server = server;
	output->usable_area = (struct wlr_box){
		0, 0, wlr_output->width, wlr_output->height
	};

	output->frame.notify = output_frame;
	wl_signal_add(&wlr_output->events.frame, &output->frame);
	output->request_state.notify = output_request_state;
	wl_signal_add(&wlr_output->events.request_state, &output->request_state);
	output->destroy.notify = output_destroy;
	wl_signal_add(&wlr_output->events.destroy, &output->destroy);

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	if (server->background_rect) {
		wlr_scene_rect_set_size(server->background_rect,
			wlr_output->width, wlr_output->height);
	}

	arrange_layers(server);
	arrange_all_workspaces(server);
}

/* --- XDG Shell Application Windows --- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, map);
	(void)data;

	struct termuxwc_server *server = toplevel->server;
	toplevel->workspace_id = server->active_workspace;
	wl_list_insert(&server->workspaces[server->active_workspace - 1].toplevels, &toplevel->link);

	arrange_workspace(server, server->active_workspace);
	focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, unmap);
	(void)data;

	struct termuxwc_server *server = toplevel->server;
	int ws_id = toplevel->workspace_id;
	struct termuxwc_workspace *ws = &server->workspaces[ws_id - 1];

	if (toplevel == server->grabbed_toplevel) {
		reset_cursor_mode(server);
	}
	if (toplevel == ws->focused) {
		ws->focused = NULL;
	}

	wl_list_remove(&toplevel->link);
	arrange_workspace(server, ws_id);

	if (ws_id == server->active_workspace) {
		if (!wl_list_empty(&ws->toplevels)) {
			struct termuxwc_toplevel *next_top =
				wl_container_of(ws->toplevels.next, next_top, link);
			focus_toplevel(next_top);
		} else {
			wlr_seat_keyboard_clear_focus(server->seat);
		}
	}
}

static void xdg_toplevel_commit(struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, commit);
	(void)data;

	if (toplevel->xdg_toplevel->base->initial_commit) {
		wlr_xdg_toplevel_set_size(toplevel->xdg_toplevel, 0, 0);
	}
}

static void server_xdg_toplevel_destroy(struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, destroy);
	(void)data;

	wl_list_remove(&toplevel->map.link);
	wl_list_remove(&toplevel->unmap.link);
	wl_list_remove(&toplevel->commit.link);
	wl_list_remove(&toplevel->destroy.link);
	wl_list_remove(&toplevel->request_move.link);
	wl_list_remove(&toplevel->request_resize.link);
	wl_list_remove(&toplevel->request_maximize.link);
	wl_list_remove(&toplevel->request_fullscreen.link);
	free(toplevel);
}

static void xdg_toplevel_request_move(
		struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_move);
	(void)data;
	begin_interactive(toplevel, TERMUXWC_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	begin_interactive(toplevel, TERMUXWC_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	(void)data;
	toggle_fullscreen(toplevel);
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	(void)data;
	toggle_fullscreen(toplevel);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct termuxwc_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	toplevel->server = server;
	toplevel->xdg_toplevel = xdg_toplevel;
	toplevel->workspace_id = server->active_workspace;
	toplevel->is_floating = false; // Tiled by default (SwayWM style)

	// Create root scene tree for window + borders
	toplevel->scene_tree = wlr_scene_tree_create(server->scene_windows);
	toplevel->scene_tree->node.data = toplevel;

	// Client surface tree inside
	struct wlr_scene_tree *surface_tree =
		wlr_scene_xdg_surface_create(toplevel->scene_tree, xdg_toplevel->base);
	xdg_toplevel->base->data = surface_tree;

	create_window_borders(toplevel);

	toplevel->map.notify = xdg_toplevel_map;
	wl_signal_add(&xdg_toplevel->base->surface->events.map, &toplevel->map);
	toplevel->unmap.notify = xdg_toplevel_unmap;
	wl_signal_add(&xdg_toplevel->base->surface->events.unmap, &toplevel->unmap);
	toplevel->commit.notify = xdg_toplevel_commit;
	wl_signal_add(&xdg_toplevel->base->surface->events.commit, &toplevel->commit);
	toplevel->destroy.notify = server_xdg_toplevel_destroy;
	wl_signal_add(&xdg_toplevel->events.destroy, &toplevel->destroy);

	toplevel->request_move.notify = xdg_toplevel_request_move;
	wl_signal_add(&xdg_toplevel->events.request_move, &toplevel->request_move);
	toplevel->request_resize.notify = xdg_toplevel_request_resize;
	wl_signal_add(&xdg_toplevel->events.request_resize, &toplevel->request_resize);
	toplevel->request_maximize.notify = xdg_toplevel_request_maximize;
	wl_signal_add(&xdg_toplevel->events.request_maximize, &toplevel->request_maximize);
	toplevel->request_fullscreen.notify = xdg_toplevel_request_fullscreen;
	wl_signal_add(&xdg_toplevel->events.request_fullscreen, &toplevel->request_fullscreen);
}

static void popup_destroy(struct wl_listener *listener, void *data) {
	struct termuxwc_popup *popup = wl_container_of(listener, popup, destroy);
	(void)data;
	wl_list_remove(&popup->destroy.link);
	wl_list_remove(&popup->commit.link);
	free(popup);
}

static void popup_commit(struct wl_listener *listener, void *data) {
	struct termuxwc_popup *popup = wl_container_of(listener, popup, commit);
	(void)data;
	if (popup->xdg_popup->base->initial_commit) {
		wlr_xdg_surface_schedule_configure(popup->xdg_popup->base);
	}
}

static void server_new_xdg_popup(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_xdg_popup);
	struct wlr_xdg_popup *xdg_popup = data;

	struct termuxwc_popup *popup = calloc(1, sizeof(*popup));
	popup->xdg_popup = xdg_popup;

	struct wlr_scene_tree *parent_tree = NULL;
	if (xdg_popup->parent != NULL) {
		struct wlr_xdg_surface *parent_surface =
			wlr_xdg_surface_try_from_wlr_surface(xdg_popup->parent);
		if (parent_surface && parent_surface->data) {
			parent_tree = parent_surface->data;
		}
	}
	if (!parent_tree) {
		parent_tree = server->scene_windows;
	}

	popup->scene_tree =
		wlr_scene_xdg_surface_create(parent_tree, xdg_popup->base);
	xdg_popup->base->data = popup->scene_tree;

	popup->commit.notify = popup_commit;
	wl_signal_add(&xdg_popup->base->surface->events.commit, &popup->commit);
	popup->destroy.notify = popup_destroy;
	wl_signal_add(&xdg_popup->events.destroy, &popup->destroy);
}

/* --- Layer Shell (Wallpaper & Status Bar) --- */

static void layer_surface_map(struct wl_listener *listener, void *data) {
	struct termuxwc_layer_surface *surface =
		wl_container_of(listener, surface, map);
	(void)data;
	arrange_layers(surface->server);
	arrange_all_workspaces(surface->server);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	struct termuxwc_layer_surface *surface =
		wl_container_of(listener, surface, unmap);
	(void)data;
	arrange_layers(surface->server);
	arrange_all_workspaces(surface->server);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct termuxwc_layer_surface *surface =
		wl_container_of(listener, surface, commit);
	(void)data;
	arrange_layers(surface->server);
}

static void layer_surface_destroy(struct wl_listener *listener, void *data) {
	struct termuxwc_layer_surface *surface =
		wl_container_of(listener, surface, destroy);
	(void)data;

	wl_list_remove(&surface->map.link);
	wl_list_remove(&surface->unmap.link);
	wl_list_remove(&surface->commit.link);
	wl_list_remove(&surface->destroy.link);
	wl_list_remove(&surface->link);
	arrange_layers(surface->server);
	arrange_all_workspaces(surface->server);
	free(surface);
}

static void server_new_layer_surface(
		struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_layer_surface);
	struct wlr_layer_surface_v1 *layer_surface = data;

	if (!layer_surface->output && !wl_list_empty(&server->outputs)) {
		struct termuxwc_output *output =
			wl_container_of(server->outputs.next, output, link);
		layer_surface->output = output->wlr_output;
	}

	struct termuxwc_layer_surface *surface = calloc(1, sizeof(*surface));
	surface->layer_surface = layer_surface;
	surface->server = server;
	layer_surface->data = surface;

	struct wlr_scene_tree *layer_tree = server->scene_bottom;
	switch (layer_surface->current.layer) {
	case ZWLR_LAYER_SHELL_V1_LAYER_BACKGROUND:
		layer_tree = server->scene_background;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_BOTTOM:
		layer_tree = server->scene_bottom;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_TOP:
		layer_tree = server->scene_top;
		break;
	case ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY:
		layer_tree = server->scene_overlay;
		break;
	}

	surface->scene_layer =
		wlr_scene_layer_surface_v1_create(layer_tree, layer_surface);

	surface->map.notify = layer_surface_map;
	wl_signal_add(&layer_surface->surface->events.map, &surface->map);
	surface->unmap.notify = layer_surface_unmap;
	wl_signal_add(&layer_surface->surface->events.unmap, &surface->unmap);
	surface->commit.notify = layer_surface_commit;
	wl_signal_add(&layer_surface->surface->events.commit, &surface->commit);
	surface->destroy.notify = layer_surface_destroy;
	wl_signal_add(&layer_surface->events.destroy, &surface->destroy);

	wl_list_insert(&server->layer_surfaces, &surface->link);
	arrange_layers(server);
}

static void arrange_layers(struct termuxwc_server *server) {
	struct termuxwc_output *output;
	wl_list_for_each(output, &server->outputs, link) {
		struct wlr_box full_area = {
			.x = 0,
			.y = 0,
			.width = output->wlr_output->width,
			.height = output->wlr_output->height,
		};
		struct wlr_box usable_area = full_area;

		struct termuxwc_layer_surface *layer;
		wl_list_for_each(layer, &server->layer_surfaces, link) {
			if (layer->layer_surface->output == output->wlr_output && layer->scene_layer) {
				wlr_scene_layer_surface_v1_configure(
					layer->scene_layer, &full_area, &usable_area);
			}
		}

		output->usable_area = usable_area;
	}
}

static void handle_terminate_signal(int sig) {
	(void)sig;
}

static void setup_xdg_runtime_dir(void) {
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	if (!runtime_dir || access(runtime_dir, W_OK) != 0) {
		const char *fallback = "/data/data/com.termux/files/usr/tmp/xdg-runtime";
		mkdir(fallback, 0700);
		setenv("XDG_RUNTIME_DIR", fallback, 1);
		wlr_log(WLR_INFO, "XDG_RUNTIME_DIR configured: %s", fallback);
	}
}

int main(int argc, char *argv[]) {
	wlr_log_init(WLR_DEBUG, NULL);

	struct termuxwc_server server = {0};
	server.default_width = 1280;
	server.default_height = 720;
	server.active_workspace = 1;

	for (int i = 0; i < NUM_WORKSPACES; i++) {
		server.workspaces[i].id = i + 1;
		wl_list_init(&server.workspaces[i].toplevels);
		server.workspaces[i].focused = NULL;
	}

	load_or_create_config(&server.config);

	char *startup_cmd = NULL;
	bool force_headless = false;

	char *custom_renderer = NULL;

	int opt;
	while ((opt = getopt(argc, argv, "s:w:h:HR:")) != -1) {
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
		case 'H':
			force_headless = true;
			break;
		case 'R':
			custom_renderer = optarg;
			break;
		default:
			fprintf(stderr, "Usage: %s [-s <cmd>] [-w <width>] [-h <height>] [-H (headless)] [-R <auto|gles2|vulkan|pixman>]\n", argv[0]);
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

	if (!force_headless) {
		server.backend = wlr_backend_autocreate(server.event_loop, NULL);
	}
	if (server.backend == NULL) {
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

	server.virtual_keyboard_mgr =
		wlr_virtual_keyboard_manager_v1_create(server.wl_display);
	server.new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&server.virtual_keyboard_mgr->events.new_virtual_keyboard,
		&server.new_virtual_keyboard);

	server.virtual_pointer_mgr =
		wlr_virtual_pointer_manager_v1_create(server.wl_display);
	server.new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&server.virtual_pointer_mgr->events.new_virtual_pointer,
		&server.new_virtual_pointer);

	wlr_primary_selection_v1_device_manager_create(server.wl_display);
	wlr_xdg_activation_v1_create(server.wl_display);
	wlr_fractional_scale_manager_v1_create(server.wl_display, 1);
	wlr_viewporter_create(server.wl_display);
	wlr_xdg_decoration_manager_v1_create(server.wl_display);
	wlr_text_input_manager_v3_create(server.wl_display);
	wlr_presentation_create(server.wl_display, server.backend, 1);

	// Output layout & Scene graph
	server.output_layout = wlr_output_layout_create(server.wl_display);
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

	wl_list_init(&server.outputs);
	server.new_output.notify = server_new_output;
	wl_signal_add(&server.backend->events.new_output, &server.new_output);

	// Shells
	server.xdg_shell = wlr_xdg_shell_create(server.wl_display, 4);
	server.new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server.xdg_shell->events.new_toplevel, &server.new_xdg_toplevel);
	server.new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server.xdg_shell->events.new_popup, &server.new_xdg_popup);

	wl_list_init(&server.layer_surfaces);
	server.layer_shell = wlr_layer_shell_v1_create(server.wl_display, 4);
	server.new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server.layer_shell->events.new_surface, &server.new_layer_surface);

	// Pointer & Cursor
	server.cursor = wlr_cursor_create();
	wlr_cursor_attach_output_layout(server.cursor, server.output_layout);
	server.cursor_mgr = wlr_xcursor_manager_create(NULL, 24);

	server.cursor_mode = TERMUXWC_CURSOR_PASSTHROUGH;
	server.cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server.cursor->events.motion, &server.cursor_motion);
	server.cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server.cursor->events.motion_absolute, &server.cursor_motion_absolute);
	server.cursor_button.notify = server_cursor_button;
	wl_signal_add(&server.cursor->events.button, &server.cursor_button);
	server.cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server.cursor->events.axis, &server.cursor_axis);
	server.cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server.cursor->events.frame, &server.cursor_frame);

	wl_list_init(&server.keyboards);
	server.new_input.notify = server_new_input;
	wl_signal_add(&server.backend->events.new_input, &server.new_input);

	server.seat = wlr_seat_create(server.wl_display, "seat0");
	server.request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server.seat->events.request_set_cursor, &server.request_cursor);
	server.request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server.seat->events.request_set_selection, &server.request_set_selection);

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
	fprintf(stderr, "Wayland socket tam yolu: %s/%s\n", xdg_runtime, socket);
	fflush(stderr);

	pid_t child_pid = -1;
	if (startup_cmd) {
		child_pid = launch_command(startup_cmd, socket);
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
	wlr_xcursor_manager_destroy(server.cursor_mgr);
	wlr_cursor_destroy(server.cursor);
	wlr_output_layout_destroy(server.output_layout);
	wlr_allocator_destroy(server.allocator);
	wlr_renderer_destroy(server.renderer);
	wlr_backend_destroy(server.backend);
	wl_display_destroy(server.wl_display);

	return 0;
}
