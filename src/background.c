/*
 * termuxwc: desktop background (wallpaper).
 *
 * The background lives in the lowest scene-graph layer, below layer-shell
 * surfaces and windows. Two modes:
 *   - solid color        (`background #rrggbb` in the config)
 *   - image wallpaper    (`background_image <path>`; decoded with libpng,
 *                        uploaded as a GPU texture via wlr_texture_from_pixels
 *                        so DRM/GLES sessions render it on the hardware)
 */

#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/background.h"
#include "termuxwc/server.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <wayland-server-core.h>
#include <wlr/render/interface.h>
#include <wlr/render/wlr_renderer.h>
#include <wlr/render/wlr_texture.h>
#include <wlr/types/wlr_scene.h>
#include <wlr/util/log.h>

#ifdef TWC_HAVE_LIBPNG
#include <png.h>
#endif

/* ------------------------- wallpaper scene node impl ---------------------- */

struct twc_wallpaper {
struct wlr_scene_node node;
struct wlr_texture *texture;
};

static void wallpaper_node_destroy(struct wlr_scene_node *scene_node) {
struct twc_wallpaper *wp = wl_container_of(scene_node, wp, node);
if (wp->texture) wlr_texture_destroy(wp->texture);
free(wp);
}

static bool wallpaper_render(struct wlr_scene_node *scene_node,
struct wlr_render_pass *render_pass) {
struct twc_wallpaper *wp = wl_container_of(scene_node, wp, node);
if (!wp->texture) return true; // nothing to draw (solid color covers us)

struct wlr_rect rect;
wlr_scene_node_get_box(scene_node, &rect);

struct wlr_render_texture_options opts = {
.texture = wp->texture,
.src_rect = (struct wlr_fbox){0},
.dst_rect = rect,
.transform = WL_OUTPUT_TRANSFORM_NORMAL,
};
wlr_render_pass_add_texture(render_pass, &opts);
return true;
}

static const struct wlr_scene_node_impl wallpaper_impl = {
.render = wallpaper_render,
.destroy = wallpaper_node_destroy,
};

/* ----------------------------- public API --------------------------------- */

void twc_background_init(struct termuxwc_server *server) {
server->background = calloc(1, sizeof(*server->background));
if (!server->background) return;
server->background->node.impl = &wallpaper_impl;
server->background->node.type = WLR_SCENE_NODE_RENDERABLE;
wlr_scene_tree_add_child(&server->scene_background->tree, INT_MIN,
&server->background->node);
}

void twc_background_set_color(struct termuxwc_server *server,
const float color[4]) {
if (server->background_rect) {
wlr_scene_rect_set_color(server->background_rect, color);
}
}

void twc_background_resize(struct termuxwc_server *server,
int width, int height) {
if (width <= 0 || height <= 0) return;
if (server->background_rect) {
wlr_scene_rect_set_size(server->background_rect, width, height);
}
if (server->background) {
wlr_scene_node_set_size(&server->background->node, width, height);
}
}

bool twc_background_set_image(struct termuxwc_server *server,
const char *path) {
#ifndef TWC_HAVE_LIBPNG
(void)server; (void)path;
wlr_log(WLR_ERROR, "background: built without libpng support;"
" using solid color instead");
return false;
#else
FILE *fp = fopen(path, "rb");
if (!fp) {
wlr_log(WLR_ERROR, "background: cannot open '%s'", path);
return false;
}

png_structp png = png_create_read_struct(PNG_LIBPNG_VER_STRING,
NULL, NULL, NULL);
png_infop info = png_create_info_struct(png);
if (!png || !info || setjmp(png_jmpbuf(png))) {
fclose(fp);
if (png) png_destroy_read_struct(&png, &info, NULL);
wlr_log(WLR_ERROR, "background: failed to decode PNG '%s'", path);
return false;
}
png_init_io(png, fp);
png_read_info(png, info);

int width = (int)png_get_image_width(png, info);
int height = (int)png_get_image_height(png, info);
png_byte color_type = png_get_color_type(png, info);
png_byte bit_depth = png_get_bit_depth(png, info);

// Normalize everything to 8-bit RGBA/BGRA (premultiplied ARGB32-ish).
if (bit_depth == 16) png_set_strip_16(png);
if (color_type == PNG_COLOR_TYPE_PALETTE) png_set_palette_to_rgb(png);
if (color_type == PNG_COLOR_TYPE_GRAY && bit_depth < 8)
png_set_expand_gray_1_2_4_to_8(png);
if (png_get_valid(png, info, PNG_INFO_tRNS)) png_set_tRNS_to_alpha(png);
if (color_type == PNG_COLOR_TYPE_RGB ||
    color_type == PNG_COLOR_TYPE_GRAY ||
    color_type == PNG_COLOR_TYPE_PALETTE)
png_set_filler(png, 0xFF, PNG_FILLER_AFTER);
if (color_type == PNG_COLOR_TYPE_GRAY ||
    color_type == PNG_COLOR_TYPE_GRAY_ALPHA)
png_set_gray_to_rgb(png);
png_read_update_info(png, info);

uint8_t *pixels = malloc((size_t)width * height * 4);
png_bytep *rows = malloc(sizeof(png_bytep) * height);
if (!pixels || !rows) {
free(pixels); free(rows);
png_destroy_read_struct(&png, &info, NULL);
fclose(fp);
return false;
}
for (int y = 0; y < height; y++) rows[y] = pixels + (size_t)y * width * 4;
png_read_rows(png, rows, NULL, height);
png_destroy_read_struct(&png, &info, NULL);
fclose(fp);

// RGBA -> BGRA (WL_SHM_FORMAT_ARGB8888 is little-endian BGRA).
for (size_t i = 0; i < (size_t)width * height; i++) {
uint8_t r = pixels[i*4+0];
pixels[i*4+0] = pixels[i*4+2];
pixels[i*4+2] = r;
}

struct wlr_texture *tex = wlr_texture_from_pixels(server->renderer,
WL_SHM_FORMAT_ARGB8888, width * 4, width, height, pixels);
free(pixels);
free(rows);
if (!tex) {
wlr_log(WLR_ERROR, "background: failed to upload wallpaper texture");
return false;
}

struct twc_wallpaper *wp = server->background;
if (wp->texture) wlr_texture_destroy(wp->texture);
wp->texture = tex;

// Hide the flat color rectangle while an image is active.
if (server->background_rect) {
wlr_scene_node_set_enabled(&server->background_rect->node, false);
}
twc_background_resize(server, server->default_width,
server->default_height);
wlr_log(WLR_INFO, "background: wallpaper '%s' (%dx%d) loaded",
path, width, height);
return true;
#endif
}

void twc_background_clear_image(struct termuxwc_server *server) {
if (server->background_rect) {
wlr_scene_node_set_enabled(&server->background_rect->node, true);
}
}
