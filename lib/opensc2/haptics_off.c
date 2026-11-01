/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_off.c: <opensc2/haptics.h> without the synthesiser (CONFIG_OPENSC2_HAPTICS_SYNTH off):
 * every call answers -ENOTSUP or does nothing, so code that plays cues or clicks builds either
 * way.
 */
#include <errno.h>
#include <string.h>
#include <opensc2/haptics.h>

int opensc2_haptics_set_output(enum opensc2_haptics_path path,
			       const struct opensc2_haptics_output *out)
{
	(void)path;
	(void)out;
	return -ENOTSUP;
}

int opensc2_haptics_set_level_output(enum opensc2_haptics_path path, opensc2_haptics_level_fn fn,
				     void *user_data)
{
	(void)path;
	(void)fn;
	(void)user_data;
	return -ENOTSUP;
}

int opensc2_haptics_play_cue(uint8_t cue, uint8_t level)
{
	(void)cue;
	(void)level;
	return -ENOTSUP;
}

int opensc2_haptics_play_script(uint8_t channels, uint8_t id, int8_t gain_db)
{
	(void)channels;
	(void)id;
	(void)gain_db;
	return -ENOTSUP;
}

int opensc2_haptics_click(uint8_t channels, uint8_t click, int8_t gain_db)
{
	(void)channels;
	(void)click;
	(void)gain_db;
	return -ENOTSUP;
}

int opensc2_haptics_stop(uint8_t channels)
{
	(void)channels;
	return -ENOTSUP;
}

int opensc2_haptics_play_clip(uint8_t channel, const uint8_t *data, uint16_t samples,
			      uint8_t format, int8_t gain_db)
{
	(void)channel;
	(void)data;
	(void)samples;
	(void)format;
	(void)gain_db;
	return -ENOTSUP;
}

void opensc2_haptics_free_fall(void)
{
}

void opensc2_haptics_free_fall_cancel(void)
{
}

int opensc2_haptics_info(struct opensc2_haptics_info *info)
{
	memset(info, 0, sizeof(*info));
	return -ENOTSUP;
}
