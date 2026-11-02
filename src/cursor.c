/*
 * termuxwc: cursor, pointer interaction and interactive move/resize.
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"

#include <linux/input-event-codes.h>

#include <wlr/types/wlr_cursor.h>
#include <wlr/types/wlr_pointer.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_xdg_shell.h>

/* --- Window Query & Interactivity --- */

struct termuxwc_toplevel *twc_desktop_toplevel_at(
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

void twc_reset_cursor_mode(struct termuxwc_server *server) {
	server->cursor_mode = TERMUXWC_CURSOR_PASSTHROUGH;
	server->grabbed_toplevel = NULL;
}

void twc_begin_interactive(struct termuxwc_toplevel *toplevel,
		enum termuxwc_cursor_mode mode, uint32_t edges) {
	struct termuxwc_server *server = toplevel->server;

	// Floating conversion on interactive drag/resize
	if (!toplevel->is_floating) {
		twc_toggle_floating(toplevel);
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
	twc_update_window_borders(toplevel, true);
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
	struct termuxwc_toplevel *toplevel = twc_desktop_toplevel_at(
		server, server->cursor->x, server->cursor->y, &surface, &sx, &sy);

	if (event->state == WL_POINTER_BUTTON_STATE_RELEASED) {
		if (server->cursor_mode != TERMUXWC_CURSOR_PASSTHROUGH) {
			twc_reset_cursor_mode(server);
		}
	} else {
		if (toplevel != NULL) {
			twc_focus_toplevel(toplevel);

			// Check Mod+Click for interactive dragging / resizing
			struct wlr_keyboard *keyboard = wlr_seat_get_keyboard(server->seat);
			uint32_t mods = keyboard ? wlr_keyboard_get_modifiers(keyboard) : 0;
			bool mod_pressed = (mods & (WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT));
			if (mod_pressed) {
				if (event->button == BTN_LEFT) {
					twc_begin_interactive(toplevel, TERMUXWC_CURSOR_MOVE, 0);
				} else if (event->button == BTN_RIGHT) {
					twc_begin_interactive(toplevel, TERMUXWC_CURSOR_RESIZE,
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

void twc_setup_cursor_listeners(struct termuxwc_server *server) {
	server->cursor_mode = TERMUXWC_CURSOR_PASSTHROUGH;
	server->cursor_motion.notify = server_cursor_motion;
	wl_signal_add(&server->cursor->events.motion, &server->cursor_motion);
	server->cursor_motion_absolute.notify = server_cursor_motion_absolute;
	wl_signal_add(&server->cursor->events.motion_absolute, &server->cursor_motion_absolute);
	server->cursor_button.notify = server_cursor_button;
	wl_signal_add(&server->cursor->events.button, &server->cursor_button);
	server->cursor_axis.notify = server_cursor_axis;
	wl_signal_add(&server->cursor->events.axis, &server->cursor_axis);
	server->cursor_frame.notify = server_cursor_frame;
	wl_signal_add(&server->cursor->events.frame, &server->cursor_frame);

	server->request_cursor.notify = seat_request_cursor;
	wl_signal_add(&server->seat->events.request_set_cursor, &server->request_cursor);
	server->request_set_selection.notify = seat_request_set_selection;
	wl_signal_add(&server->seat->events.request_set_selection, &server->request_set_selection);
}
