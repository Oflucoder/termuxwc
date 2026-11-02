#define _POSIX_C_SOURCE 200809L

#include "termuxwc/common.h"

#include <stdio.h>
#include <string.h>

void twc_parse_hex_color(const char *hex, float color[4]) {
	if (hex[0] == '#') hex++;
	unsigned int r = 0, g = 0, b = 0;
	if (sscanf(hex, "%02x%02x%02x", &r, &g, &b) == 3) {
		color[0] = r / 255.0f;
		color[1] = g / 255.0f;
		color[2] = b / 255.0f;
		color[3] = 1.0f;
	}
}
