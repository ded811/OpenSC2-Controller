/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * haptics_out_pwm.c: the pads' blocks played by an nRF PWM instance as a two-channel "DAC"
 * (CONFIG_OPENSC2_HAPTICS_OUT_PWM_DAC), on nrfx's sequence playback, set up from the
 * "opensc2,haptics-pwm-dac" devicetree node. As on the real controller, the 16 MHz clock counts to
 * 256 (a 16 us period) and each 8-bit sample is the duty cycle itself, 0..255 steps of 256, held
 * for 16 periods: 256 us a sample, so the pads play about 3906 samples a second rather than 4000
 * (their tones about 2.3 % flat, their sounds that much longer). With
 * CONFIG_OPENSC2_HAPTICS_OUT_PWM_DAC_EXACT_4K the clock counts to 250 instead (a 15.625 us period,
 * the sample scaled to 0..250 steps): exactly 4 kHz. The PWM loads two values per step, the left
 * pad's for channels 0 and 1 (its pin on 0) and the right pad's for 2 and 3 (its pin on 2). Two
 * sequences of one block each play in turn, looped: while one plays, the other is filled with the
 * next block, which a write (or ready()) waits for. Idle, both outputs hold half duty (silence), as
 * on the real controller. Written for stock nrfx; not Valve's driver. Built; not yet tested on
 * hardware.
 */
#include <errno.h>
#include <string.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/init.h>
#include <zephyr/irq.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <nrfx_pwm.h>
#include <soc.h>
#include <opensc2/haptics.h>

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

#define NODE DT_INST(0, opensc2_haptics_pwm_dac)
#define PWM_NODE DT_PHANDLE(NODE, pwm)
#if defined(CONFIG_OPENSC2_HAPTICS_OUT_PWM_DAC_EXACT_4K)
#define TOP 250u                /* 16 MHz / 250 = 64 kHz PWM: 250 us a sample, exactly 4 kHz */
#else
#define TOP 256u                /* 16 MHz / 256 = 62.5 kHz PWM: 256 us a sample, as the real one */
#endif
#define REPEATS 15u             /* each value 16 periods */
/* Bit 15 of a value is the pin's polarity: set, the pin is high for the value's counts of each
 * period (Zephyr's PWM driver sets it so for normal polarity); clear, the pin would be low for
 * them, and both pads would come out inverted. */
#define HIGH_FOR_VALUE 0x8000u
#define SILENCE (HIGH_FOR_VALUE | (TOP / 2u))
#define WAIT_MS 40

BUILD_ASSERT(!DT_NODE_HAS_STATUS(PWM_NODE, okay),
	     "opensc2,haptics-pwm-dac: leave its PWM instance's node disabled (nrfx drives it, not "
	     "Zephyr's PWM driver)");

static nrfx_pwm_t pwm = NRFX_PWM_INSTANCE(DT_REG_ADDR(PWM_NODE));
static nrf_pwm_values_grouped_t seq_values[2][OPENSC2_HAPTICS_PAD_FRAMES];
static nrf_pwm_values_grouped_t idle_value[1] = { { SILENCE, SILENCE } };
static K_SEM_DEFINE(free_seqs, 2, 2);
static uint8_t silence[OPENSC2_HAPTICS_PAD_FRAMES * 2];
static uint8_t next_fill;
static bool playing;
static bool reserved;           /* ready() took a free sequence for the next write() */
#if DT_NODE_HAS_PROP(NODE, enable_gpios)
static const struct gpio_dt_spec drv_en = GPIO_DT_SPEC_GET(NODE, enable_gpios);
#endif

static const nrf_pwm_sequence_t seqs[2] = {
	{ .values.p_grouped = seq_values[0], .length = NRF_PWM_VALUES_LENGTH(seq_values[0]),
	  .repeats = REPEATS, .end_delay = 0 },
	{ .values.p_grouped = seq_values[1], .length = NRF_PWM_VALUES_LENGTH(seq_values[1]),
	  .repeats = REPEATS, .end_delay = 0 },
};
static const nrf_pwm_sequence_t idle_seq = {
	.values.p_grouped = idle_value, .length = NRF_PWM_VALUES_LENGTH(idle_value),
	.repeats = 0, .end_delay = 0,
};

/* In the PWM's interrupt: a sequence's last value was loaded, so its buffer may be refilled. */
static void pwm_event(nrfx_pwm_event_type_t event, void *ctx)
{
	ARG_UNUSED(ctx);
	if (event == NRFX_PWM_EVENT_END_SEQ0 || event == NRFX_PWM_EVENT_END_SEQ1) {
		k_sem_give(&free_seqs);
	}
}

/* A sample as steps of high time: the value itself of 256 steps (128 is half duty), or with the
 * exact 4 kHz option 0..255 scaled to 0..250, rounded. */
static uint16_t duty(uint8_t v)
{
	return (uint16_t)(HIGH_FOR_VALUE | (TOP == 256u ? v : (v * TOP + 127u) / 255u));
}

static void fill(uint8_t which, const uint8_t *block)
{
	for (int f = 0; f < OPENSC2_HAPTICS_PAD_FRAMES; f++) {
		seq_values[which][f].group_0 = duty(block[f * 2]);
		seq_values[which][f].group_1 = duty(block[f * 2 + 1]);
	}
}

static void idle(void)
{
	/* half duty on both outputs, looped by the PWM itself (no interrupts) */
	(void)nrfx_pwm_simple_playback(&pwm, &idle_seq, 1, NRFX_PWM_FLAG_LOOP);
}

static int pwm_out_start(const struct opensc2_haptics_output *out)
{
	ARG_UNUSED(out);
	(void)nrfx_pwm_stop(&pwm, true);
	playing = true;
	(void)nrfx_pwm_complex_playback(&pwm, &seqs[0], &seqs[1], 1,
					NRFX_PWM_FLAG_LOOP | NRFX_PWM_FLAG_SIGNAL_END_SEQ0 |
					NRFX_PWM_FLAG_SIGNAL_END_SEQ1);
	return 0;
}

static int pwm_out_ready(const struct opensc2_haptics_output *out)
{
	ARG_UNUSED(out);
	/* Both sequences hold blocks that have not played: wait for the older one to finish. (A late
	 * block cannot be told from the hardware: the loop then replays the old one once.) */
	if (!reserved) {
		if (k_sem_take(&free_seqs, K_MSEC(WAIT_MS)) != 0) {
			return -EPIPE;
		}
		reserved = true;
	}
	return 0;
}

static int pwm_out_write(const struct opensc2_haptics_output *out, const void *block,
			 size_t frames)
{
	int err;

	if (frames != OPENSC2_HAPTICS_PAD_FRAMES) {
		return -EINVAL;
	}
	err = pwm_out_ready(out);
	if (err) {
		return err;
	}
	reserved = false;
	fill(next_fill, block);
	next_fill ^= 1u;
	return 0;
}

static int pwm_out_stop(const struct opensc2_haptics_output *out, bool drop)
{
	if (playing && !drop) {
		/* A block of silence behind the last one, then wait until the last one has played. */
		(void)pwm_out_write(out, silence, OPENSC2_HAPTICS_PAD_FRAMES);
		(void)k_sem_take(&free_seqs, K_MSEC(WAIT_MS));
	}
	(void)nrfx_pwm_stop(&pwm, true);
	playing = false;
	reserved = false;
	k_sem_reset(&free_seqs);
	k_sem_give(&free_seqs);
	k_sem_give(&free_seqs);
	next_fill = 0;
	idle();
	return 0;
}

static void pwm_out_enable(const struct opensc2_haptics_output *out, bool on)
{
	ARG_UNUSED(out);
#if DT_NODE_HAS_PROP(NODE, enable_gpios)
	(void)gpio_pin_set_dt(&drv_en, on ? 1 : 0);
#else
	ARG_UNUSED(on);
#endif
}

static const struct opensc2_haptics_output_api pwm_out_api = {
	.start = pwm_out_start,
	.ready = pwm_out_ready,
	.write = pwm_out_write,
	.stop = pwm_out_stop,
	.enable = pwm_out_enable,
};

static const struct opensc2_haptics_output pwm_out = { .api = &pwm_out_api };

static int pwm_out_init(void)
{
	nrfx_pwm_config_t cfg = NRFX_PWM_DEFAULT_CONFIG(NRF_DT_GPIOS_TO_PSEL(NODE, left_gpios),
							NRF_PWM_PIN_NOT_CONNECTED,
							NRF_DT_GPIOS_TO_PSEL(NODE, right_gpios),
							NRF_PWM_PIN_NOT_CONNECTED);
	int err;

	memset(silence, 128, sizeof(silence));
	cfg.irq_priority = DT_IRQ(PWM_NODE, priority);
	cfg.base_clock = NRF_PWM_CLK_16MHz;
	cfg.count_mode = NRF_PWM_MODE_UP;
	cfg.top_value = TOP;
	cfg.load_mode = NRF_PWM_LOAD_GROUPED;
	cfg.step_mode = NRF_PWM_STEP_AUTO;
	IRQ_CONNECT(DT_IRQN(PWM_NODE), DT_IRQ(PWM_NODE, priority), nrfx_pwm_irq_handler, &pwm, 0);
#if DT_NODE_HAS_PROP(NODE, enable_gpios)
	if (!gpio_is_ready_dt(&drv_en) || gpio_pin_configure_dt(&drv_en, GPIO_OUTPUT_INACTIVE)) {
		LOG_ERR("Haptics: the pads' enable pin cannot be set up");
		return 0;
	}
#endif
	err = nrfx_pwm_init(&pwm, &cfg, pwm_event, NULL);
	if (err) {
		LOG_ERR("Haptics: the pads' PWM cannot be set up (%d)", err);
		return 0;
	}
	idle();
	return opensc2_haptics_set_output(OPENSC2_HAPTICS_PADS, &pwm_out);
}

SYS_INIT(pwm_out_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
