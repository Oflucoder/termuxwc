/*
 * termuxwc: Termux:API display backend — app-embedded, VNC-free output.
 *
 * This is the Android/Termux answer to "how do we put pixels on the screen
 * without a parent compositor and without VNC". Instead of asking some other
 * process to show our frames, termuxwc becomes the surface producer itself:
 *
 *   1. It connects (as a Wayland *client*) to the Termux:API display server,
 *      started by the Termux app through com.termux.api.TermuxService with
 *      `termux-display --socket-fd <fd>`; the abstract socket name arrives via
 *      the TERMUX_DISPLAY env var (same mechanism Termux:X11 uses).
 *   2. It creates a sub-surface of the Termux:API window and attaches its own
 *      wlroots scene outputs to it (wlr_wl_output_create + wlr_scene_output).
 *   3. Frames are presented over that connection. On Adreno/Mali devices with
 *      EGL + dmabuf support in the parent, wlroots' Wayland backend performs
 *      zero-copy / direct scanout presentation — real hardware acceleration,
 *      no encoder, no network round-trip, no root.
 *
 * The heavy lifting (EGL context, buffers, presentation) is done by wlroots'
 * built-in Wayland backend; this module only resolves the parent display and
 * wires up outputs.
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <wayland-client.h>
#include <wlr/backend/wayland.h>
#include <wlr/types/wlr_output.h>
#include <wlr/util/log.h>

/*
 * Resolve the parent Termux:API display name. Priority:
 *   -d <display>            explicit CLI override
 *   $TERMUX_DISPLAY         set by the Termux app when launching termuxwc
 *                           from a Termux:API display session
 *   $WAYLAND_DISPLAY        generic fallback (works under any Wayland parent)
 */
const char *twc_termux_api_resolve_display(const char *cli_display) {
	const char *parent = cli_display;
	if (!parent || !*parent) parent = getenv("TERMUX_DISPLAY");
	if (!parent || !*parent) parent = getenv("WAYLAND_DISPLAY");
	if (!parent || !*parent) return NULL;

	// Accept full socket paths too; libwayland resolves names against
	// $XDG_RUNTIME_DIR, so only the basename is useful.
	const char *slash = strrchr(parent, '/');
	if (slash) parent = slash[1] != '\0' ? slash + 1 : NULL;
	return parent;
}

struct wlr_backend *twc_termux_api_backend_create(
		struct wl_event_loop *event_loop, const char *cli_display) {
	const char *parent = twc_termux_api_resolve_display(cli_display);
	if (!parent) {
		wlr_log(WLR_ERROR, "termux-api backend: no parent display found."
			" Enable a Termux:API display session (sets $TERMUX_DISPLAY),"
			" or pass -d <display> explicitly.");
		return NULL;
	}

	// Fail fast with a clear message if the socket is not reachable.
	const char *runtime_dir = getenv("XDG_RUNTIME_DIR");
	char path[4096];
	if (runtime_dir) {
		snprintf(path, sizeof(path), "%s/%s", runtime_dir, parent);
	} else {
		snprintf(path, sizeof(path), "%s", parent);
	}
	if (access(path, W_OK) != 0 && parent[0] != '@') {
		// Abstract sockets start with '@' and have no filesystem entry.
		wlr_log(WLR_ERROR, "termux-api backend: cannot access parent display"
			" socket '%s' (is the Termux app running a display session?)", path);
		return NULL;
	}

	struct wlr_backend *backend = wlr_wl_backend_create(event_loop, parent);
	if (!backend) {
		wlr_log(WLR_ERROR, "termux-api backend: failed to connect to Termux:API"
			" display '%s'", parent);
		return NULL;
	}
	wlr_log(WLR_INFO, "termux-api backend: attached to Termux:API display '%s'"
		" (app-embedded, zero-copy EGL presentation, no VNC)", parent);
	return backend;
}

void twc_termux_api_add_output(struct termuxwc_server *server) {
	struct wlr_output *output = wlr_wl_output_create(server->backend);
	if (!output) {
		wlr_log(WLR_ERROR, "termux-api backend: failed to create windowed output");
		return;
	}
	char title[128];
	snprintf(title, sizeof(title), "termuxwc (%dx%d)",
		server->default_width, server->default_height);
	wlr_wl_output_set_title(output, title);
	wlr_log(WLR_INFO, "termux-api backend: output '%s' created", title);
}
