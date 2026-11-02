/*
 * termuxwc: dynamic master-stack tiling engine.
 *
 * Layout rules (Sway-style, orientation-adaptive):
 * - 1 window   -> fills the usable area.
 * - 2 windows  -> split evenly (side-by-side in landscape, stacked in portrait).
 * - 3+ windows -> master/stack: master takes half the area, the remaining
 *                 windows share the other half evenly.
 * - Fullscreen windows claim the whole usable area; floating windows keep
 *   their saved geometry.
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"

#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_xdg_shell.h>

/* Maximum number of tiled windows considered per arrange pass. */
#define TILED_WINDOW_LIMIT 128

/* --- Window Borders & Decorations --- */

void twc_create_window_borders(struct termuxwc_toplevel *toplevel) {
	float *color = toplevel->server->config.color_inactive_border;
	toplevel->border_top = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
	toplevel->border_bottom = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
	toplevel->border_left = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
	toplevel->border_right = wlr_scene_rect_create(toplevel->scene_tree, 0, 0, color);
}

void twc_update_window_borders(struct termuxwc_toplevel *toplevel, bool focused) {
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

void twc_arrange_workspace(struct termuxwc_server *server, int ws_id) {
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
			twc_update_window_borders(toplevel, toplevel == ws->focused);
			return; // Fullscreen window takes over entire active display
		}
	}

	// Collect tiled windows
	int num_tiled = 0;
	struct termuxwc_toplevel *tiled[TILED_WINDOW_LIMIT];
	wl_list_for_each(toplevel, &ws->toplevels, link) {
		if (!toplevel->is_floating) {
			if (num_tiled < TILED_WINDOW_LIMIT) tiled[num_tiled++] = toplevel;
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
			twc_update_window_borders(toplevel, toplevel == ws->focused);
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
		twc_update_window_borders(w, w == ws->focused);
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
				twc_update_window_borders(tiled[i], tiled[i] == ws->focused);
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
				twc_update_window_borders(tiled[i], tiled[i] == ws->focused);
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
			twc_update_window_borders(tiled[0], tiled[0] == ws->focused);

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
				twc_update_window_borders(tiled[i], tiled[i] == ws->focused);
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
			twc_update_window_borders(tiled[0], tiled[0] == ws->focused);

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
				twc_update_window_borders(tiled[i], tiled[i] == ws->focused);
			}
		}
	}
}

void twc_arrange_all_workspaces(struct termuxwc_server *server) {
	for (int i = 1; i <= NUM_WORKSPACES; i++) {
		twc_arrange_workspace(server, i);
	}
}
