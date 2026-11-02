/*
 * termuxwc: XDG shell toplevel management, workspaces and focus.
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"

#include <stdlib.h>

#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>
#include <wlr/util/log.h>

/* --- Workspace & Focus Actions --- */

void twc_focus_toplevel(struct termuxwc_toplevel *toplevel) {
	if (!toplevel) return;
	struct termuxwc_server *server = toplevel->server;
	struct termuxwc_workspace *ws = &server->workspaces[toplevel->workspace_id - 1];
	struct wlr_seat *seat = server->seat;

	struct wlr_surface *prev_surface = seat->keyboard_state.focused_surface;
	struct wlr_surface *surface = toplevel->xdg_toplevel->base->surface;

	if (ws->focused && ws->focused != toplevel) {
		twc_update_window_borders(ws->focused, false);
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
	twc_update_window_borders(toplevel, true);

	struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(seat);
	if (keyboard != NULL) {
		wlr_seat_keyboard_notify_enter(seat, surface,
			keyboard->keycodes, keyboard->num_keycodes, &keyboard->modifiers);
	}
}

void twc_switch_workspace(struct termuxwc_server *server, int target_ws) {
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
	twc_arrange_workspace(server, target_ws);

	struct termuxwc_workspace *cur = &server->workspaces[target_ws - 1];
	if (cur->focused) {
		twc_focus_toplevel(cur->focused);
	} else if (!wl_list_empty(&cur->toplevels)) {
		struct termuxwc_toplevel *first =
			wl_container_of(cur->toplevels.next, first, link);
		twc_focus_toplevel(first);
	} else {
		wlr_seat_keyboard_clear_focus(server->seat);
	}

	wlr_log(WLR_INFO, "Switched to Workspace %d", target_ws);
}

void twc_move_toplevel_to_workspace(struct termuxwc_toplevel *toplevel, int target_ws) {
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

	twc_arrange_workspace(server, old_ws);
	twc_arrange_workspace(server, target_ws);

	struct termuxwc_workspace *cur_ws = &server->workspaces[server->active_workspace - 1];
	if (!wl_list_empty(&cur_ws->toplevels)) {
		struct termuxwc_toplevel *next_top =
			wl_container_of(cur_ws->toplevels.next, next_top, link);
		twc_focus_toplevel(next_top);
	} else {
		cur_ws->focused = NULL;
		wlr_seat_keyboard_clear_focus(server->seat);
	}

	wlr_log(WLR_INFO, "Moved window to Workspace %d", target_ws);
}

void twc_toggle_floating(struct termuxwc_toplevel *toplevel) {
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
	twc_arrange_workspace(toplevel->server, toplevel->workspace_id);
}

void twc_toggle_fullscreen(struct termuxwc_toplevel *toplevel) {
	if (!toplevel) return;
	toplevel->is_fullscreen = !toplevel->is_fullscreen;
	wlr_xdg_toplevel_set_fullscreen(toplevel->xdg_toplevel, toplevel->is_fullscreen);
	twc_arrange_workspace(toplevel->server, toplevel->workspace_id);
}

void twc_close_active_window(struct termuxwc_server *server) {
	struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
	if (ws->focused && ws->focused->xdg_toplevel) {
		wlr_xdg_toplevel_send_close(ws->focused->xdg_toplevel);
	}
}

void twc_focus_next_window(struct termuxwc_server *server, bool reverse) {
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
	twc_focus_toplevel(target);
}

void twc_swap_next_window(struct termuxwc_server *server, bool reverse) {
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

	twc_arrange_workspace(server, server->active_workspace);
	twc_focus_toplevel(cur);
}

/* --- XDG Shell Application Windows --- */

static void xdg_toplevel_map(struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, map);
	(void)data;

	struct termuxwc_server *server = toplevel->server;
	toplevel->workspace_id = server->active_workspace;
	wl_list_insert(&server->workspaces[server->active_workspace - 1].toplevels, &toplevel->link);

	twc_arrange_workspace(server, server->active_workspace);
	twc_focus_toplevel(toplevel);
}

static void xdg_toplevel_unmap(struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, unmap);
	(void)data;

	struct termuxwc_server *server = toplevel->server;
	int ws_id = toplevel->workspace_id;
	struct termuxwc_workspace *ws = &server->workspaces[ws_id - 1];

	if (toplevel == server->grabbed_toplevel) {
		twc_reset_cursor_mode(server);
	}
	if (toplevel == ws->focused) {
		ws->focused = NULL;
	}

	wl_list_remove(&toplevel->link);
	twc_arrange_workspace(server, ws_id);

	if (ws_id == server->active_workspace) {
		if (!wl_list_empty(&ws->toplevels)) {
			struct termuxwc_toplevel *next_top =
				wl_container_of(ws->toplevels.next, next_top, link);
			twc_focus_toplevel(next_top);
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
	twc_begin_interactive(toplevel, TERMUXWC_CURSOR_MOVE, 0);
}

static void xdg_toplevel_request_resize(
		struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_resize);
	struct wlr_xdg_toplevel_resize_event *event = data;
	twc_begin_interactive(toplevel, TERMUXWC_CURSOR_RESIZE, event->edges);
}

static void xdg_toplevel_request_maximize(
		struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_maximize);
	(void)data;
	twc_toggle_fullscreen(toplevel);
}

static void xdg_toplevel_request_fullscreen(
		struct wl_listener *listener, void *data) {
	struct termuxwc_toplevel *toplevel =
		wl_container_of(listener, toplevel, request_fullscreen);
	(void)data;
	twc_toggle_fullscreen(toplevel);
}

static void server_new_xdg_toplevel(struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_xdg_toplevel);
	struct wlr_xdg_toplevel *xdg_toplevel = data;

	struct termuxwc_toplevel *toplevel = calloc(1, sizeof(*toplevel));
	if (!toplevel) {
		wlr_log(WLR_ERROR, "Out of memory allocating toplevel");
		return;
	}
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

	twc_create_window_borders(toplevel);

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
	if (!popup) {
		wlr_log(WLR_ERROR, "Out of memory allocating popup");
		return;
	}
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

void twc_setup_xdg_shell(struct termuxwc_server *server) {
	server->xdg_shell = wlr_xdg_shell_create(server->wl_display, 4);
	server->new_xdg_toplevel.notify = server_new_xdg_toplevel;
	wl_signal_add(&server->xdg_shell->events.new_toplevel, &server->new_xdg_toplevel);
	server->new_xdg_popup.notify = server_new_xdg_popup;
	wl_signal_add(&server->xdg_shell->events.new_popup, &server->new_xdg_popup);
}
