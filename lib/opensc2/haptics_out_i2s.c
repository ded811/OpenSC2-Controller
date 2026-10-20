/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_out_i2s.c: the grips' blocks to an I2S amplifier (CONFIG_OPENSC2_HAPTICS_OUT_I2S), set up
 * from the "opensc2,haptics-i2s" devicetree node: Zephyr's I2S driver as master, 16-bit stereo at
 * 8 kHz (the left grip on the left word, as the synthesiser's frames are laid out), blocks of 64
 * frames from a memory slab of two, so a write waits until the oldest block has played. An
 * underrun (the I2S driver's error state) is answered with -EPIPE; the synthesiser then stops
 * (drops) and starts again. Built; not yet tested on hardware.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/i2s.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <opensc2/haptics.h>

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define NODE DT_INST(0, opensc2_haptics_i2s)
#define BLOCK_BYTES (OPENSC2_HAPTICS_GRIP_FRAMES * 2 * sizeof(int16_t))
#define WAIT_MS 40      /* a block plays in 8 ms: a write waits at most this long */

static const struct device *const i2s_dev = DEVICE_DT_GET(DT_PHANDLE(NODE, i2s));
#if DT_NODE_HAS_PROP(NODE, enable_gpios)
static const struct gpio_dt_spec amp_en = GPIO_DT_SPEC_GET(NODE, enable_gpios);
#endif

K_MEM_SLAB_DEFINE_STATIC(i2s_blocks, BLOCK_BYTES, 2, 4);

static int i2s_out_start(const struct opensc2_haptics_output *out)
{
	ARG_UNUSED(out);
	return i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_START);
}

static int i2s_out_write(const struct opensc2_haptics_output *out, const void *block,
			 size_t frames)
{
	size_t bytes = frames * 2 * sizeof(int16_t);
	void *mem;
	int err;

	ARG_UNUSED(out);
	if (bytes > BLOCK_BYTES) {
		return -EINVAL;
	}
	/* Both blocks queued: wait for the older one to have played. */
	err = k_mem_slab_alloc(&i2s_blocks, &mem, K_MSEC(WAIT_MS));
	if (err) {
		return -EPIPE;
	}
	memcpy(mem, block, bytes);
	err = i2s_write(i2s_dev, mem, bytes);
	if (err) {
		k_mem_slab_free(&i2s_blocks, mem);
		/* -EIO: the driver ran out of blocks before this one and stopped (an underrun) */
		return err == -EIO ? -EPIPE : err;
	}
	return 0;
}

static int i2s_out_stop(const struct opensc2_haptics_output *out, bool drop)
{
	void *mem[2];
	int err;

	ARG_UNUSED(out);
	if (drop) {
		/* After an underrun the driver is in its error state: PREPARE brings it back. */
		err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DROP);
		if (err) {
			err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_PREPARE);
		}
		return err;
	}
	err = i2s_trigger(i2s_dev, I2S_DIR_TX, I2S_TRIGGER_DRAIN);
	/* Drained once both blocks are back in the slab. */
	for (int i = 0; i < 2; i++) {
		if (k_mem_slab_alloc(&i2s_blocks, &mem[i], K_MSEC(2 * WAIT_MS)) != 0) {
			mem[i] = NULL;
		}
	}
	for (int i = 0; i < 2; i++) {
		if (mem[i]) {
			k_mem_slab_free(&i2s_blocks, mem[i]);
		}
	}
	return err;
}

static void i2s_out_enable(const struct opensc2_haptics_output *out, bool on)
{
	ARG_UNUSED(out);
#if DT_NODE_HAS_PROP(NODE, enable_gpios)
	(void)gpio_pin_set_dt(&amp_en, on ? 1 : 0);
#else
	ARG_UNUSED(on);
#endif
}

static const struct opensc2_haptics_output_api i2s_out_api = {
	.start = i2s_out_start,
	.write = i2s_out_write,
	.stop = i2s_out_stop,
	.enable = i2s_out_enable,
};

static const struct opensc2_haptics_output i2s_out = { .api = &i2s_out_api };

static int i2s_out_init(void)
{
	struct i2s_config cfg = {
		.word_size = 16,
		.channels = 2,
		.format = I2S_FMT_DATA_FORMAT_I2S,
		.options = I2S_OPT_BIT_CLK_CONTROLLER | I2S_OPT_FRAME_CLK_CONTROLLER,
		.frame_clk_freq = OPENSC2_HAPTICS_GRIP_RATE_HZ,
		.mem_slab = &i2s_blocks,
		.block_size = BLOCK_BYTES,
		.timeout = WAIT_MS,
	};
	int err;

	if (!device_is_ready(i2s_dev)) {
		LOG_ERR("Haptics: the I2S device is not ready");
		return 0;
	}
#if DT_NODE_HAS_PROP(NODE, enable_gpios)
	if (!gpio_is_ready_dt(&amp_en) || gpio_pin_configure_dt(&amp_en, GPIO_OUTPUT_INACTIVE)) {
		LOG_ERR("Haptics: the amplifier's enable pin cannot be set up");
		return 0;
	}
#endif
	err = i2s_configure(i2s_dev, I2S_DIR_TX, &cfg);
	if (err) {
		LOG_ERR("Haptics: I2S at 8 kHz, 16-bit stereo refused (%d)", err);
		return 0;
	}
	return opensc2_haptics_set_output(OPENSC2_HAPTICS_GRIPS, &i2s_out);
}

SYS_INIT(i2s_out_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
