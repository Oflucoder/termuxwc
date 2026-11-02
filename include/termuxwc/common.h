/*
 * termuxwc: shared utility declarations.
 */
#ifndef TERMUXWC_COMMON_H
#define TERMUXWC_COMMON_H

#include <stdbool.h>

/* Parse a "#rrggbb" (or "rrggbb") hex color into a normalized RGBA float array. */
void twc_parse_hex_color(const char *hex, float color[4]);

#endif /* TERMUXWC_COMMON_H */
