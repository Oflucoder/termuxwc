/*
 * termuxwc: configuration loading & startup helpers.
 */
#ifndef TERMUXWC_CONFIG_H
#define TERMUXWC_CONFIG_H

#include <sys/types.h>

#include "termuxwc/server.h"

/* Load ~/.config/termuxwc/config, creating a documented default file if missing. */
void twc_load_or_create_config(struct termuxwc_config *cfg);

/* Fork/exec the given shell command with WAYLAND_DISPLAY set to `socket`. */
pid_t twc_launch_command(const char *cmd, const char *socket);

#endif /* TERMUXWC_CONFIG_H */
