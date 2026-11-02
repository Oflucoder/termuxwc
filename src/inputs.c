/*
 * termuxwc: keyboard handling, Sway-style keybindings and input hotplugging.
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"

#include <stdlib.h>

#include <wlr/backend.h>
#include <wlr/types/wlr_input_device.h>
#include <wlr/types/wlr_keyboard.h>
#include <wlr/types/wlr_seat.h>
#include <wlr/types/wlr_virtual_keyboard_v1.h>
#include <wlr/types/wlr_virtual_pointer_v1.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

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
				twc_launch_command(server->config.terminal_cmd, socket);
				handled = true;
				break;
			}
			// Mod + D: Launch App Launcher
			else if (sym == XKB_KEY_d || sym == XKB_KEY_D) {
				twc_launch_command(server->config.launcher_cmd, socket);
				handled = true;
				break;
			}
			// Mod + Q or Mod + Shift + C: Close Window
			else if (sym == XKB_KEY_q || sym == XKB_KEY_Q ||
			         (shift && (sym == XKB_KEY_c || sym == XKB_KEY_C))) {
				twc_close_active_window(server);
				handled = true;
				break;
			}
			// Mod + F: Toggle Fullscreen
			else if (sym == XKB_KEY_f || sym == XKB_KEY_F) {
				struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
				if (ws->focused) twc_toggle_fullscreen(ws->focused);
				handled = true;
				break;
			}
			// Mod + Space: Toggle Floating
			else if (sym == XKB_KEY_space) {
				struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
				if (ws->focused) twc_toggle_floating(ws->focused);
				handled = true;
				break;
			}
			// Mod + H/J/K/L or Arrows: Directional Focus / Swap
			else if (sym == XKB_KEY_j || sym == XKB_KEY_J || sym == XKB_KEY_Down ||
			         sym == XKB_KEY_l || sym == XKB_KEY_L || sym == XKB_KEY_Right) {
				if (shift) twc_swap_next_window(server, false);
				else twc_focus_next_window(server, false);
				handled = true;
				break;
			}
			else if (sym == XKB_KEY_k || sym == XKB_KEY_K || sym == XKB_KEY_Up ||
			         sym == XKB_KEY_h || sym == XKB_KEY_H || sym == XKB_KEY_Left) {
				if (shift) twc_swap_next_window(server, true);
				else twc_focus_next_window(server, true);
				handled = true;
				break;
			}
			// Mod + 1..9: Workspace Switching & Window Moving
			else if (sym >= XKB_KEY_1 && sym <= XKB_KEY_9) {
				int target_ws = sym - XKB_KEY_1 + 1;
				if (shift) {
					struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
					if (ws->focused) twc_move_toplevel_to_workspace(ws->focused, target_ws);
				} else {
					twc_switch_workspace(server, target_ws);
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
	if (!keyboard) {
		wlr_log(WLR_ERROR, "Out of memory allocating keyboard");
		return;
	}
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

	// Refocus active window with new keyboard
	struct termuxwc_workspace *ws = &server->workspaces[server->active_workspace - 1];
	if (ws->focused) {
		twc_focus_toplevel(ws->focused);
	} else if (!wl_list_empty(&ws->toplevels)) {
		struct termuxwc_toplevel *top =
			wl_container_of(ws->toplevels.next, top, link);
		twc_focus_toplevel(top);
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

static void update_seat_capabilities(struct termuxwc_server *server) {
	uint32_t caps = WL_SEAT_CAPABILITY_POINTER;
	if (!wl_list_empty(&server->keyboards)) {
		caps |= WL_SEAT_CAPABILITY_KEYBOARD;
	}
	wlr_seat_set_capabilities(server->seat, caps);
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

	update_seat_capabilities(server);
}

static void server_new_virtual_keyboard(
		struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_virtual_keyboard);
	struct wlr_virtual_keyboard_v1 *virtual_keyboard = data;
	setup_keyboard_device(server, &virtual_keyboard->keyboard);
	update_seat_capabilities(server);
}

static void server_new_virtual_pointer(
		struct wl_listener *listener, void *data) {
	struct termuxwc_server *server =
		wl_container_of(listener, server, new_virtual_pointer);
	struct wlr_virtual_pointer_v1_new_pointer_event *event = data;

	wlr_cursor_attach_input_device(
		server->cursor, &event->new_pointer->pointer.base);

	update_seat_capabilities(server);
}

void twc_setup_input(struct termuxwc_server *server) {
	wl_list_init(&server->keyboards);
	server->new_input.notify = server_new_input;
	wl_signal_add(&server->backend->events.new_input, &server->new_input);

	server->virtual_keyboard_mgr =
		wlr_virtual_keyboard_manager_v1_create(server->wl_display);
	server->new_virtual_keyboard.notify = server_new_virtual_keyboard;
	wl_signal_add(&server->virtual_keyboard_mgr->events.new_virtual_keyboard,
		&server->new_virtual_keyboard);

	server->virtual_pointer_mgr =
		wlr_virtual_pointer_manager_v1_create(server->wl_display);
	server->new_virtual_pointer.notify = server_new_virtual_pointer;
	wl_signal_add(&server->virtual_pointer_mgr->events.new_virtual_pointer,
		&server->new_virtual_pointer);
}
