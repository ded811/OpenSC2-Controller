/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * A Steam Controller 2 of your own, on the OpenSC2 library. Start here.
 *
 * The library does the protocols: the USB device that Steam pairs over the cable (wired mode) and
 * the radio link through Valve's puck (wireless mode). This file does the hardware: it hands the
 * buttons of the overlay (boards/<board>.overlay) to the library and shows the state on the
 * board's LED. Every function and callback: docs/api.md in the OpenSC2-Controller repository.
 *
 * Modes, as on the real controller: a boot with USB power is wired. Steam pairs the board with its
 * puck over the cable and reboots it into wireless mode; it then connects through the puck. The
 * pairing button can choose too (pair_button() below).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/input/input.h>
#include <zephyr/logging/log.h>
#include <opensc2/opensc2.h>

LOG_MODULE_REGISTER(my_controller, LOG_LEVEL_INF);

/* The pairing button (labelled opensc2_pair in the overlay) and the status LED (the board's led0).
 * On a board without one of them the spec stays empty, and the code below leaves it alone. */
static const struct gpio_dt_spec pair_pin =
	GPIO_DT_SPEC_GET_OR(DT_NODELABEL(opensc2_pair), gpios, {0});
static const struct gpio_dt_spec led = GPIO_DT_SPEC_GET_OR(DT_ALIAS(led0), gpios, {0});

/* ---- the pairing button ---------------------------------------------------------------------- */

static bool pair_held_at_boot;   /* held at power-up: chose this boot's mode, ignored until up */
static bool pair_pressed;        /* wired: a press was seen, so its release counts */

/* At power-up, before opensc2_init(). gpio-keys has already set the pin up as an input with its
 * pull-up. Read it twice, 5 ms apart: a bouncing contact counts as released. */
static bool pair_pin_held(void)
{
	if (!gpio_is_ready_dt(&pair_pin) || gpio_pin_get_dt(&pair_pin) != 1) {
		return false;
	}
	k_msleep(5);
	return gpio_pin_get_dt(&pair_pin) == 1;
}

static void pair_button(bool pressed)
{
	if (pair_held_at_boot) {
		pair_held_at_boot = pressed;   /* released: a normal button from now on */
		return;
	}
	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		/* Pressed and released while wired: reboot into wireless mode. Acting on the
		 * release keeps the button up at the next boot, so the warm-restart mark brings
		 * the board back wireless, not a held button. A release without a press (a pin
		 * settling at power-up) does nothing. */
		if (pressed) {
			pair_pressed = true;
		} else if (pair_pressed) {
			pair_pressed = false;
			/* _later: from the system work queue, so this callback returns at once. */
			opensc2_reboot_later(OPENSC2_REBOOT_WIRELESS, "pair button", 0);
		}
	} else if (pressed) {
		/* Wireless: answer a puck whose pairing window is open over the air (untested with
		 * Steam, which pairs controllers over the cable). */
		LOG_INF("Pairing: answering an open pairing window");
		opensc2_pair_start(0);
	}
}

/* ---- buttons ---------------------------------------------------------------------------------
 * Every event of the opensc2_buttons node, in Zephyr's input thread. zephyr,code says what the
 * button is (dt-bindings/opensc2/inputs.h).
 */
static void on_button(struct input_event *evt, void *user_data)
{
	bool pressed = evt->value != 0;

	ARG_UNUSED(user_data);
	if (evt->type != INPUT_EV_KEY) {
		return;
	}
#if !defined(CONFIG_OPENSC2_SYSTEM)
	/* Without the system state machine: after the puck turned the board off (the PC went to
	 * sleep), any button brings it back. With the machine (the default) the real controller's
	 * rules decide instead: on USB power with wireless off, STEAM reboots the board, which comes
	 * back wired, and the other buttons do nothing ('opensc2 start' turns wireless on again). */
	if (pressed && opensc2_mode() == OPENSC2_MODE_WIRELESS &&
	    opensc2_link_state() == OPENSC2_LINK_OFF) {
		opensc2_start();
	}
#endif

	switch (evt->code) {
	case OPENSC2_KEY_PAIR:
		pair_button(pressed);
		break;
	case OPENSC2_KEY_LEFT_TRIGGER_CLICK:
		/* A digital trigger: pressed is the trigger pulled all the way. The library sets
		 * the trigger's click bit from the value (CONFIG_OPENSC2_TRIGGER_CLICK_AUTO). */
		opensc2_set_trigger(OPENSC2_LEFT, pressed ? OPENSC2_TRIGGER_MAX : 0);
		break;
	case OPENSC2_KEY_RIGHT_TRIGGER_CLICK:
		opensc2_set_trigger(OPENSC2_RIGHT, pressed ? OPENSC2_TRIGGER_MAX : 0);
		break;
	default:
		/* Any other code below 32 is a bit of the button word: change just that bit. */
		if (evt->code < 32) {
			opensc2_set_buttons(BIT(evt->code), pressed ? BIT(evt->code) : 0);
		}
		break;
	}
}
INPUT_CALLBACK_DEFINE(DEVICE_DT_GET(DT_NODELABEL(opensc2_buttons)), on_button, NULL);

/* ---- library callbacks -----------------------------------------------------------------------
 * The library calls these from its own threads (the radio thread while wireless, the USB threads
 * while wired). Return quickly, well under a millisecond, and hand anything slow to a thread or a
 * work queue of your own. Every callback is optional.
 */
static void on_link(const struct opensc2_link_info *info, void *user_data)
{
	ARG_UNUSED(user_data);
	if (info->reason == OPENSC2_REASON_PAIRED) {
		/* The API numbers the two pairing slots 0 and 1, the shell 1 and 2. */
		LOG_INF("Paired with a puck (slot %u)", info->slot + 1);
	} else if (info->state == OPENSC2_LINK_OFF &&
		   info->reason == OPENSC2_REASON_PUCK_TURN_OFF) {
		LOG_INF("Turned off by the puck (the PC sleeps): %s",
			IS_ENABLED(CONFIG_OPENSC2_SYSTEM) ?
			"STEAM reboots the board wired; opensc2 start turns wireless on again" :
			"press a button to come back");
	}
}

/* Outputs arrive the same way. A rumble motor, for example (samples/rumble drives one):
 *
 *	static void on_rumble(const struct opensc2_rumble *rumble, void *user_data)
 *	{
 *		my_motor_set(MAX(rumble->left_speed, rumble->right_speed));   (0 stops it)
 *	}
 *
 * with .rumble = on_rumble below. Haptics, the LED colour Steam sets and the controller settings
 * have callbacks too (docs/api.md, Outputs).
 *
 * There is no power_request callback: without one the library carries out the reboots Steam asks
 * for itself (among them Steam's "reboot into wireless mode" after it paired the board over the
 * cable) and ignores Steam's request to enter a bootloader for a firmware update, which the UF2
 * bootloader could not take anyway. Add one to decide yourself; app/src/main.c has a complete one.
 */
static const struct opensc2_callbacks callbacks = {
	.link_changed = on_link,
	/* .rumble = on_rumble, */
};

/* ---- the status LED --------------------------------------------------------------------------
 * Called every 50 ms: on while wired or connected through the puck, a slow blink while looking
 * for the puck, a short blink every 2 s without a pairing, otherwise off.
 */
#define LED_TICK_MS 50

static void show_status(uint32_t tick)
{
	bool on = false;

	if (opensc2_mode() == OPENSC2_MODE_WIRED) {
		on = true;
	} else {
		switch (opensc2_link_state()) {
		case OPENSC2_LINK_CONNECTED:
			on = true;
			break;
		case OPENSC2_LINK_LISTENING:
			on = tick % 20 < 4;    /* 200 ms on, every second */
			break;
		case OPENSC2_LINK_NO_BOND:
			on = tick % 40 == 0;   /* 50 ms on, every 2 s */
			break;
		default:
			break;                 /* pairing over the air, or the radio off */
		}
	}
	gpio_pin_set_dt(&led, on);
}

int main(void)
{
	struct opensc2_config config = {
		.callbacks = &callbacks,
		.bond_slot = OPENSC2_SLOT_STORED,   /* the pairing slot chosen last */
		.start = true,                      /* wireless: start the radio at once */
		/* .identity stays NULL: the Kconfig identity, with development serials ("OSC...")
		 * made from the chip's ID. */
	};
	bool have_led = gpio_is_ready_dt(&led) &&
			gpio_pin_configure_dt(&led, GPIO_OUTPUT_INACTIVE) == 0;
	int err;

	/* Held at power-up, the pairing button makes this boot wireless, even on USB power. */
	if (pair_pin_held()) {
		LOG_INF("Pairing button held at power-up: wireless this time");
		pair_held_at_boot = true;
		config.boot_mode = OPENSC2_BOOT_WIRELESS;
	}

	/* Load the bonds and the identity, decide this boot's mode, then start the USB device
	 * (wired) or the radio (wireless). */
	err = opensc2_init(&config);
	if (err) {
		/* For example -EINVAL for an invalid identity. The serial console still starts, so
		 * this message can be read there. */
		LOG_ERR("opensc2_init failed (%d)", err);
		return err;
	}
	LOG_INF("Started %s", opensc2_mode() == OPENSC2_MODE_WIRED ? "wired" : "wireless");

	for (uint32_t tick = 0;; tick++) {
		if (have_led) {
			show_status(tick);
		}
		k_msleep(LED_TICK_MS);
	}
	return 0;
}
