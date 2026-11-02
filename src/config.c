#define _POSIX_C_SOURCE 200809L
#define _WLR_USE_UNSTABLE

#include "termuxwc/config.h"
#include "termuxwc/common.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <wlr/backend.h>
#include <wlr/util/log.h>
#include <xkbcommon/xkbcommon.h>

void twc_load_or_create_config(struct termuxwc_config *cfg) {
	// Sane defaults
	cfg->mod_mask = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT; // Support both Super & Alt!
	cfg->inner_gap = 6;
	cfg->outer_gap = 8;
	cfg->border_width = 2;
	snprintf(cfg->terminal_cmd, sizeof(cfg->terminal_cmd), "foot");
	snprintf(cfg->launcher_cmd, sizeof(cfg->launcher_cmd), "foot -e fzf");
	twc_parse_hex_color("#7aa2f7", cfg->color_active_border);    // TokyoNight blue
	twc_parse_hex_color("#3b4261", cfg->color_inactive_border);  // Subtle slate
	twc_parse_hex_color("#16161e", cfg->color_background);       // Deep dark background

	const char *home = getenv("HOME");
	if (!home) return;

	char config_dir[512], config_path[512];
	snprintf(config_dir, sizeof(config_dir), "%s/.config/termuxwc", home);
	snprintf(config_path, sizeof(config_path), "%s/config", config_dir);

	mkdir(config_dir, 0755);

	FILE *f = fopen(config_path, "r");
	if (!f) {
		// Generate clean documented default config
		f = fopen(config_path, "w");
		if (f) {
			fprintf(f, "# termuxwc Sway-grade Tiling Window Manager Configuration\n");
			fprintf(f, "# Mod key: Mod4 = Super/Windows key, Mod1 = Alt key, ModBoth = both\n");
			fprintf(f, "mod ModBoth\n\n");
			fprintf(f, "# Application commands\n");
			fprintf(f, "terminal foot\n");
			fprintf(f, "launcher foot -e fzf\n\n");
			fprintf(f, "# Geometry & Gaps (pixels)\n");
			fprintf(f, "inner_gap 6\n");
			fprintf(f, "outer_gap 8\n");
			fprintf(f, "border_width 2\n\n");
			fprintf(f, "# Colors (Hex)\n");
			fprintf(f, "active_border #7aa2f7\n");
			fprintf(f, "inactive_border #3b4261\n");
			fprintf(f, "background #16161e\n");
			fclose(f);
		}
		return;
	}

	char line[256];
	while (fgets(line, sizeof(line), f)) {
		char *p = line;
		while (isspace((unsigned char)*p)) p++;
		if (*p == '#' || *p == '\0') continue;

		char key[64], val[192];
		if (sscanf(p, "%63s %191[^\n]", key, val) == 2) {
			if (strcmp(key, "terminal") == 0) {
				snprintf(cfg->terminal_cmd, sizeof(cfg->terminal_cmd), "%s", val);
			} else if (strcmp(key, "launcher") == 0) {
				snprintf(cfg->launcher_cmd, sizeof(cfg->launcher_cmd), "%s", val);
			} else if (strcmp(key, "inner_gap") == 0) {
				cfg->inner_gap = atoi(val);
			} else if (strcmp(key, "outer_gap") == 0) {
				cfg->outer_gap = atoi(val);
			} else if (strcmp(key, "border_width") == 0) {
				cfg->border_width = atoi(val);
			} else if (strcmp(key, "active_border") == 0) {
				twc_parse_hex_color(val, cfg->color_active_border);
			} else if (strcmp(key, "inactive_border") == 0) {
				twc_parse_hex_color(val, cfg->color_inactive_border);
			} else if (strcmp(key, "background") == 0) {
				twc_parse_hex_color(val, cfg->color_background);
			} else if (strcmp(key, "mod") == 0) {
				if (strcmp(val, "Mod4") == 0) cfg->mod_mask = WLR_MODIFIER_LOGO;
				else if (strcmp(val, "Mod1") == 0) cfg->mod_mask = WLR_MODIFIER_ALT;
				else cfg->mod_mask = WLR_MODIFIER_LOGO | WLR_MODIFIER_ALT;
			}
		}
	}
	fclose(f);
}

pid_t twc_launch_command(const char *cmd, const char *socket) {
	pid_t pid = fork();
	if (pid < 0) {
		wlr_log_errno(WLR_ERROR, "fork failed for command");
		return -1;
	}
	if (pid == 0) {
		setenv("WAYLAND_DISPLAY", socket, 1);
		execl("/bin/sh", "sh", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	return pid;
}
