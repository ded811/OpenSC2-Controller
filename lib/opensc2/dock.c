/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock.c: the dock on Zephyr.
 *   - Always with the dock link: what dock_link.c needs from the kernel (a state report built now,
 *     a microsecond clock), the board's inputs (the application's readers, <opensc2/docking.h>,
 *     or the devicetree's), the pilot check and the supply rule on them: a fresh pilot check each
 *     time "a supply is present" changes decides between the dock's power and USB power, as on
 *     the real controller.
 *   - With CONFIG_OPENSC2_DOCK, BUILT BUT NEVER RUN ON HARDWARE: the pogo UART and the pilot
 *     sense of the devicetree node aliased opensc2-dock (dts/bindings/opensc2,dock.yaml). One
 *     thread checks the pilot between sessions (three readings 3 ms apart, two inside 2000 to
 *     3300 mV; 10 ms between checks that fail), and the first check that passes starts a session:
 *     arm the receiver for one 73-byte frame (no limit for the first byte; a 64 ms silence ends
 *     the frame early), answer it, wait until the answer has left, and arm again. Any frame that
 *     fails ends the session, and the thread goes back to the pilot. The receiver is never armed
 *     while the answer goes out, so the controller cannot hear its own echo.
 *
 * Nothing may be connected to Valve's puck's pads without the owner's step-by-step go-ahead: the
 * pads' voltages and loads have not been measured (docs/testing.md, The dock).
 */
#include <errno.h>
#include <string.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/spinlock.h>
#include <opensc2/docking.h>
#include "internal.h"
#include "dock_link.h"
#include "test_dock.h"

#if defined(CONFIG_OPENSC2_DOCK)
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/gpio.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/init.h>
#endif

LOG_MODULE_DECLARE(opensc2, CONFIG_OPENSC2_LOG_LEVEL);

/* ---- what the dock link needs from the kernel ------------------------------------------------ */

void osc2_dock_sample(uint8_t report[OPENSC2_STATE_REPORT_LEN])
{
	struct opensc2_state s;

	/* Lizard mode never runs on a report the dock asked for: nothing of it goes to the dock. */
	osc2_report_state(report, &s);
}

uint32_t osc2_dock_now_us(void)
{
	return (uint32_t)k_ticks_to_us_floor64((uint64_t)k_uptime_ticks());
}

/* ---- the board's inputs: the application's readers, or the devicetree's ---------------------- */

/* One pilot check at a time (the dock thread's and the supply rule's). */
static K_MUTEX_DEFINE(check_lock);

static int32_t dt_pilot_mv(void);
static bool dt_pilot_ready(void);
static int dt_plus(void);

static struct opensc2_dock_board board_now(void)
{
	struct opensc2_dock_board b;

	osc2_dock_board(&b);
	return b;
}

static bool have_pilot(void)
{
	return board_now().pilot_mv != NULL || dt_pilot_ready();
}

/* One reading in mV at the pin, or a negative value when it failed (it then counts as outside). */
static int32_t pilot_reading(void)
{
	struct opensc2_dock_board b = board_now();

	if (b.pilot_mv) {
		return b.pilot_mv(b.user);
	}
	return dt_pilot_mv();
}

/* The dock's + on a sense input: 1, 0, or negative when nothing senses it (VBUS does). */
static int plus_reading(void)
{
	struct opensc2_dock_board b = board_now();

	if (b.plus) {
		return b.plus(b.user);
	}
	return dt_plus();
}

/* One check: three readings, 3 ms after each; present when two are inside the window. */
static bool pilot_check(void)
{
	int32_t mv[OSC2_DOCK_PILOT_READS];
	uint8_t inside;
	bool present;

	(void)k_mutex_lock(&check_lock, K_FOREVER);
	for (int i = 0; i < OSC2_DOCK_PILOT_READS; i++) {
		mv[i] = pilot_reading();
		k_sleep(K_MSEC(OSC2_DOCK_PILOT_READ_MS));
	}
	(void)k_mutex_unlock(&check_lock);
	present = osc2_dock_pilot_present(mv, &inside);
	if (present) {
		osc2_test_note_dock_pilot(mv, inside, present);   /* not every failed check */
	}
	osc2_dock_pilot_seen(present);
	return present;
}

int opensc2_dock_pilot_check(void)
{
	if (!have_pilot()) {
		return -ENODEV;
	}
	return pilot_check() ? 1 : 0;
}

/* The supply rule's readings come from the system work queue (and the power-on decision); one at
 * a time, as the check in it sleeps. */
static K_MUTEX_DEFINE(supply_lock);

enum osc2_dock_supply osc2_dock_supply(void)
{
	enum osc2_dock_supply kind;
	int plus;

	if (!have_pilot()) {
		return OSC2_DOCK_SUPPLY_NONE;   /* no pilot to check: the dock never powers the board */
	}
	(void)k_mutex_lock(&supply_lock, K_FOREVER);
	plus = plus_reading();
	kind = osc2_dock_supply_step(osc2_usb_power(), plus > 0, pilot_check);
	(void)k_mutex_unlock(&supply_lock);
	return kind;
}

#if !defined(CONFIG_OPENSC2_DOCK)
static int32_t dt_pilot_mv(void)
{
	return -ENODEV;
}

static bool dt_pilot_ready(void)
{
	return false;
}

static int dt_plus(void)
{
	return -ENODEV;
}
#endif


#if defined(CONFIG_OPENSC2_DOCK)

/* ---- the UART and the pilot (CONFIG_OPENSC2_DOCK) -------------------------------------------- */

#define DOCK_NODE DT_ALIAS(opensc2_dock)

BUILD_ASSERT(DT_NODE_HAS_STATUS(DOCK_NODE, okay),
	     "CONFIG_OPENSC2_DOCK needs the devicetree alias opensc2-dock (an \"opensc2,dock\" node)");

static const struct device *const uart = DEVICE_DT_GET(DT_PHANDLE(DOCK_NODE, uart));

/* The pilot: an ADC channel (io-channels), or a window comparator's output (pilot-gpios, active
 * while the pilot is inside its window), as the real controller's ADC-window line. */
#define DOCK_PILOT_ADC  DT_NODE_HAS_PROP(DOCK_NODE, io_channels)
#define DOCK_PILOT_GPIO DT_NODE_HAS_PROP(DOCK_NODE, pilot_gpios)
BUILD_ASSERT(DOCK_PILOT_ADC || DOCK_PILOT_GPIO,
	     "the opensc2,dock node needs its pilot: io-channels or pilot-gpios");
BUILD_ASSERT(!DOCK_PILOT_ADC || IS_ENABLED(CONFIG_ADC),
	     "the opensc2,dock node's io-channels need CONFIG_ADC");
#if DOCK_PILOT_ADC
static const struct adc_dt_spec pilot = ADC_DT_SPEC_GET(DOCK_NODE);
#else
static const struct gpio_dt_spec pilot_line = GPIO_DT_SPEC_GET(DOCK_NODE, pilot_gpios);
#endif
static bool pilot_ready;
/* The dock's + on a sense input of its own (optional): without one VBUS says it, as on the real
 * controller, whose + reaches its supply input. */
static const struct gpio_dt_spec plus = GPIO_DT_SPEC_GET_OR(DOCK_NODE, plus_gpios, { 0 });
static bool plus_ready;

static uint8_t rx_buf[OPENSC2_DOCK_FRAME_LEN];
static uint8_t tx_buf[OPENSC2_DOCK_FRAME_LEN];
static volatile size_t rx_len;
static volatile bool rx_error;
static K_SEM_DEFINE(rx_done, 0, 1);
static K_SEM_DEFINE(tx_done, 0, 1);

static void uart_cb(const struct device *dev, struct uart_event *evt, void *user_data)
{
	ARG_UNUSED(user_data);
	switch (evt->type) {
	case UART_RX_RDY:
		rx_len = evt->data.rx.offset + evt->data.rx.len;
		if (rx_len < sizeof(rx_buf)) {
			/* The 64 ms silence inside a frame: the frame ends here (too short). */
			(void)uart_rx_disable(dev);
		}
		break;
	case UART_RX_STOPPED:
		rx_error = true;   /* framing, break or overrun: the driver stops the reception */
		break;
	case UART_RX_DISABLED:
		k_sem_give(&rx_done);
		break;
	case UART_TX_DONE:
	case UART_TX_ABORTED:
		k_sem_give(&tx_done);
		break;
	default:
		break;
	}
}

static bool dt_pilot_ready(void)
{
	return pilot_ready;
}

#if DOCK_PILOT_ADC
/* One conversion in mV at the pin, or -1 when it fails (it then counts as outside). */
static int32_t adc_mv(void)
{
	int16_t sample = 0;
	int32_t mv;
	struct adc_sequence seq = {
		.buffer = &sample,
		.buffer_size = sizeof(sample),
	};

	if (adc_sequence_init_dt(&pilot, &seq) != 0 || adc_read_dt(&pilot, &seq) != 0) {
		return -1;
	}
	mv = sample < 0 ? 0 : sample;
	if (adc_raw_to_millivolts_dt(&pilot, &mv) != 0) {
		return -1;
	}
	return mv;
}
#endif

/* One reading in mV at the pin; negative when it failed (it then counts as outside). A window
 * comparator's line reads as the window's middle while active, 0 otherwise. */
static int32_t dt_pilot_mv(void)
{
	if (!pilot_ready) {
		return -ENODEV;
	}
#if DOCK_PILOT_ADC
	return adc_mv();
#else
	int v = gpio_pin_get_dt(&pilot_line);

	return v < 0 ? -1 : (v ? (OSC2_DOCK_PILOT_MIN_MV + OSC2_DOCK_PILOT_MAX_MV) / 2 : 0);
#endif
}

static int dt_plus(void)
{
	if (!plus_ready) {
		return -ENODEV;   /* VBUS says it */
	}
	return gpio_pin_get_dt(&plus) > 0 ? 1 : 0;
}

/* Before the application starts: the pilot and the + sense set up, so that the power-on decision
 * in opensc2_init() can check the pilot (the supply rule checks it as the first reading posts). */
static int dock_boot_setup(void)
{
	if (plus.port != NULL && gpio_is_ready_dt(&plus) &&
	    gpio_pin_configure_dt(&plus, GPIO_INPUT) == 0) {
		plus_ready = true;
	}
#if DOCK_PILOT_ADC
	pilot_ready = adc_is_ready_dt(&pilot) && adc_channel_setup_dt(&pilot) == 0;
#else
	pilot_ready = gpio_is_ready_dt(&pilot_line) &&
		      gpio_pin_configure_dt(&pilot_line, GPIO_INPUT) == 0;
#endif
	if (!pilot_ready) {
		LOG_ERR("Dock: the pilot input could not be set up");
	}
	return 0;
}
SYS_INIT(dock_boot_setup, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

/* Arm the receiver for one frame and wait for it, without a time limit. Returns the bytes that
 * arrived, or a negative errno when the receiver could not be armed. */
static int receive_frame(void)
{
	int err;

	rx_len = 0;
	rx_error = false;
	k_sem_reset(&rx_done);
	err = uart_rx_enable(uart, rx_buf, sizeof(rx_buf),
			     OPENSC2_DOCK_GAP_TIMEOUT_MS * USEC_PER_MSEC);
	if (err) {
		return err;
	}
	(void)k_sem_take(&rx_done, K_FOREVER);
	return (int)rx_len;
}

static void session(void)
{
	for (;;) {
		int n = receive_frame();
		enum osc2_dock_outcome outcome;

		if (n < 0) {
			LOG_WRN("Dock: the receiver could not be armed (%d)", n);
			osc2_dock_session_end(OSC2_DOCK_END_ARM_FAILED);
			return;
		}
		if (rx_error && n != OPENSC2_DOCK_FRAME_LEN) {
			/* A framing, break or overrun error cut the frame short. The session ends
			 * as for any short frame, with the receive error as its reason; the frame
			 * is still counted and recorded (as too short). */
			LOG_WRN("Dock: receive error after %d bytes", n);
			osc2_dock_session_end(OSC2_DOCK_END_RX_ERROR);
			(void)osc2_dock_receive(rx_buf, (size_t)n, tx_buf, NULL);
			return;
		}
		outcome = osc2_dock_receive(rx_buf, (size_t)n, tx_buf, NULL);
		if (outcome != OSC2_DOCK_ANSWERED) {
			return;   /* the dock link ended the session */
		}
		k_sem_reset(&tx_done);
		if (uart_tx(uart, tx_buf, sizeof(tx_buf), SYS_FOREVER_US) != 0) {
			/* A send that fails skips this answer; the session goes on. */
			LOG_WRN("Dock: the answer could not be sent");
			osc2_dock_after_answer();
			continue;
		}
		/* About 6.3 ms on the wire. Meanwhile a GET input with the dock alone builds its
		 * report; the receiver is armed again only after the answer has left. */
		osc2_dock_after_answer();
		if (k_sem_take(&tx_done, K_MSEC(50)) != 0) {
			/* Stuck (it should never be): stopped, so that the next answer can go
			 * out and the receiver is not armed while this one may be on the line. */
			LOG_WRN("Dock: the answer did not leave within 50 ms");
			(void)uart_tx_abort(uart);
			(void)k_sem_take(&tx_done, K_MSEC(10));
		}
		if (!osc2_dock_active()) {
			return;   /* ended from elsewhere (the test hooks, a shutdown) */
		}
	}
}

static void dock_thread(void *a, void *b, void *c)
{
	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);

	/* The identity and the command channel exist once the application has called
	 * opensc2_init(); a probe before that would answer an empty record. */
	while (!osc2.initialised) {
		k_sleep(K_MSEC(10));
	}
	if (!device_is_ready(uart) || uart_callback_set(uart, uart_cb, NULL) != 0) {
		LOG_ERR("Dock: the UART or its async API is not ready");
		return;
	}
	LOG_INF("Dock: checking the pilot (UNTESTED on hardware)");
	for (;;) {
		struct osc2_dock_info info;

		osc2_dock_info(&info);
		if (info.active && info.starter == OSC2_DOCK_BY_TEST) {
			/* The test hooks feed the dock link frames of their own: leave the UART alone
			 * until their session ends (with their arming at the latest). */
			k_sleep(K_MSEC(OSC2_DOCK_PILOT_IDLE_MS));
			continue;
		}
		if (!info.active) {
			if (!have_pilot() || !pilot_check()) {
				k_sleep(K_MSEC(OSC2_DOCK_PILOT_IDLE_MS));
				continue;
			}
			osc2_dock_session_start(OSC2_DOCK_BY_PILOT);
		}
		session();
	}
}

K_THREAD_DEFINE(osc2_dock_tid, CONFIG_OPENSC2_DOCK_THREAD_STACK_SIZE, dock_thread, NULL, NULL,
		NULL, CONFIG_OPENSC2_DOCK_THREAD_PRIORITY, 0, 0);

#endif /* CONFIG_OPENSC2_DOCK */
