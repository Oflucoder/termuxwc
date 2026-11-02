/*
 * termuxwc: desktop background API.
 */
#ifndef TERMUXWC_BACKGROUND_H
#define TERMUXWC_BACKGROUND_H

#include <stdbool.h>

#include "termuxwc/server.h"

/* Create the wallpaper scene node in the background layer. */
void twc_background_init(struct termuxwc_server *server);

/* Solid-color mode (config `background #rrggbb`). */
void twc_background_set_color(struct termuxwc_server *server, const float color[4]);

/* Resize all background nodes (called per output). */
void twc_background_resize(struct termuxwc_server *server, int width, int height);

/* Image mode (config `background_image <png>`). Returns false on failure —
 * caller keeps the solid color fallback. */
bool twc_background_set_image(struct termuxwc_server *server, const char *path);

/* Re-enable the flat color rectangle. */
void twc_background_clear_image(struct termuxwc_server *server);

#endif /* TERMUXWC_BACKGROUND_H */
