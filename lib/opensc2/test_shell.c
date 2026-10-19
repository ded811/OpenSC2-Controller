/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * test_shell.c: "opensc2 test [<id>] <verb> [<arguments>]", the hardware test hooks on the console
 * (wireless mode). The verbs and every line they print are in test_core.c (osc2_test_console());
 * this only hands the shell's words over and prints the lines.
 */
#include <zephyr/shell/shell.h>
#include "test_hooks.h"

/* The longest line the test suite sends: "opensc2 test 65535 raw " and 122 hex digits. */
BUILD_ASSERT(CONFIG_SHELL_CMD_BUFF_SIZE >= 160,
	     "opensc2 test needs CONFIG_SHELL_CMD_BUFF_SIZE of 160 or more: a raw request of 61 bytes "
	     "makes a 145-character line");

static void emit(void *ctx, const char *line)
{
	shell_print((const struct shell *)ctx, "%s", line);
}

int osc2_test_shell(const struct shell *sh, size_t argc, char **argv)
{
	return osc2_test_console_run((int)argc - 1, (const char *const *)&argv[1], emit, (void *)sh);
}
