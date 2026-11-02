/*
 * termuxwc: output (display) management & frame scheduling.
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"
#include "termuxwc/background.h"

#include <stdlib.h>
#include <time.h>

#include <wayland-server-core.h>
#include <wlr/backend.h>
#include <wlr/types/wlr_output.h>
#include <wlr/types/wlr_output_layout.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

/* Fallback refresh rate when none is configured (mHz). */
#define DEFAULT_REFRESH_MHZ 120000

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
	wl_display_flush_clients(output->server->wl_display);
}

static void output_request_state(struct wl_listener *listener, void *data) {
	struct termuxwc_output *output =
		wl_container_of(listener, output, request_state);
	const struct wlr_output_event_request_state *event = data;
	wlr_output_commit_state(output->wlr_output, event->state);

	output->usable_area = (struct wlr_box){
		0, 0, output->wlr_output->width, output->wlr_output->height
	};

	twc_arrange_layers(output->server);
	twc_arrange_all_workspaces(output->server);
}

/*
 * Timer-driven frame pump for headless/VNC sessions where the backend does
 * not generate its own frame events. Only commits when the scene actually
 * changed (needs_frame), avoiding pointless GPU/CPU work on idle screens.
 */
static int output_frame_timer_handler(void *data) {
	struct termuxwc_output *output = data;
	struct timespec now;
	clock_gettime(CLOCK_MONOTONIC, &now);

	struct wlr_scene *scene = output->server->scene;
	struct wlr_scene_output *scene_output =
		wlr_scene_get_scene_output(scene, output->wlr_output);
	if (scene_output) {
		if (wlr_scene_output_needs_frame(scene_output)) {
			wlr_scene_output_commit(scene_output, NULL);
		}
		wlr_scene_output_send_frame_done(scene_output, &now);
	}

	// Signal frame done for active toplevel surfaces
	if (output->server->active_workspace >= 1 &&
	    output->server->active_workspace <= NUM_WORKSPACES) {
		struct termuxwc_workspace *ws =
			&output->server->workspaces[output->server->active_workspace - 1];
		struct termuxwc_toplevel *top;
		wl_list_for_each(top, &ws->toplevels, link) {
			if (top->xdg_toplevel && top->xdg_toplevel->base &&
			    top->xdg_toplevel->base->surface) {
				wlr_surface_send_frame_done(top->xdg_toplevel->base->surface, &now);
			}
		}
	}

	wl_display_flush_clients(output->server->wl_display);

	int interval_ms = twc_output_frame_interval_ms(output->server);
	wl_event_source_timer_update(output->frame_timer, interval_ms);
	return 0;
}

int twc_output_frame_interval_ms(const struct termuxwc_server *server) {
	int hz = server->default_refresh_rate > 0
		? server->default_refresh_rate / 1000
		: DEFAULT_REFRESH_MHZ / 1000;
	if (hz < 30) hz = 30;
	int interval_ms = 1000 / hz;
	if (interval_ms < 1) interval_ms = 8;
	return interval_ms;
}

static void output_destroy(struct wl_listener *listener, void *data) {
	struct termuxwc_output *output =
		wl_container_of(listener, output, destroy);
	(void)data;
	if (output->frame_timer) {
		wl_event_source_remove(output->frame_timer);
	}
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
			server->default_width, server->default_height,
			server->default_refresh_rate > 0
				? server->default_refresh_rate : DEFAULT_REFRESH_MHZ);
	}

	wlr_output_commit_state(wlr_output, &state);
	wlr_output_state_finish(&state);

	wlr_output_lock_software_cursors(wlr_output, true);

	struct termuxwc_output *output = calloc(1, sizeof(*output));
	if (!output) {
		wlr_log(WLR_ERROR, "Out of memory allocating output");
		return;
	}
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

	output->frame_timer = wl_event_loop_add_timer(server->event_loop,
		output_frame_timer_handler, output);
	wl_event_source_timer_update(output->frame_timer,
		twc_output_frame_interval_ms(server));

	wl_list_insert(&server->outputs, &output->link);

	struct wlr_output_layout_output *l_output =
		wlr_output_layout_add_auto(server->output_layout, wlr_output);
	struct wlr_scene_output *scene_output =
		wlr_scene_output_create(server->scene, wlr_output);
	wlr_scene_output_layout_add_output(server->scene_layout, l_output, scene_output);

	twc_background_resize(server, wlr_output->width, wlr_output->height);

	twc_arrange_layers(server);
	twc_arrange_all_workspaces(server);
}

void twc_setup_outputs(struct termuxwc_server *server) {
	wl_list_init(&server->outputs);
	server->new_output.notify = server_new_output;
	wl_signal_add(&server->backend->events.new_output, &server->new_output);
}
