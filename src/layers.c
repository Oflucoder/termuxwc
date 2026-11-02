/*
 * termuxwc: Layer-Shell v1 support (wallpapers, status bars, overlays).
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/server.h"

#include <stdlib.h>

#include <wlr/types/wlr_layer_shell_v1.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

void twc_arrange_layers(struct termuxwc_server *server) {
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

static void layer_surface_map(struct wl_listener *listener, void *data) {
	struct termuxwc_layer_surface *surface =
		wl_container_of(listener, surface, map);
	(void)data;
	twc_arrange_layers(surface->server);
	twc_arrange_all_workspaces(surface->server);
}

static void layer_surface_unmap(struct wl_listener *listener, void *data) {
	struct termuxwc_layer_surface *surface =
		wl_container_of(listener, surface, unmap);
	(void)data;
	twc_arrange_layers(surface->server);
	twc_arrange_all_workspaces(surface->server);
}

static void layer_surface_commit(struct wl_listener *listener, void *data) {
	struct termuxwc_layer_surface *surface =
		wl_container_of(listener, surface, commit);
	(void)data;
	twc_arrange_layers(surface->server);
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
	twc_arrange_layers(surface->server);
	twc_arrange_all_workspaces(surface->server);
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
	if (!surface) {
		wlr_log(WLR_ERROR, "Out of memory allocating layer surface");
		return;
	}
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
	twc_arrange_layers(server);
}

void twc_setup_layer_shell(struct termuxwc_server *server) {
	wl_list_init(&server->layer_surfaces);
	server->layer_shell = wlr_layer_shell_v1_create(server->wl_display, 4);
	server->new_layer_surface.notify = server_new_layer_surface;
	wl_signal_add(&server->layer_shell->events.new_surface, &server->new_layer_surface);
}
