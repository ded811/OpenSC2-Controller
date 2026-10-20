/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * dock.c: the dock on Zephyr.
 *   - Always with the dock link: what dock_link.c needs from the kernel (a state report built now,
 *     a microsecond clock).
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
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include "internal.h"
#include "dock_link.h"
#include "test_dock.h"

#if defined(CONFIG_OPENSC2_DOCK)
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/adc.h>
#include <zephyr/drivers/uart.h>
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

#if defined(CONFIG_OPENSC2_DOCK)

/* ---- the UART and the pilot (CONFIG_OPENSC2_DOCK) -------------------------------------------- */

#define DOCK_NODE DT_ALIAS(opensc2_dock)

BUILD_ASSERT(DT_NODE_HAS_STATUS(DOCK_NODE, okay),
	     "CONFIG_OPENSC2_DOCK needs the devicetree alias opensc2-dock (an \"opensc2,dock\" node)");

static const struct device *const uart = DEVICE_DT_GET(DT_PHANDLE(DOCK_NODE, uart));
static const struct adc_dt_spec pilot = ADC_DT_SPEC_GET(DOCK_NODE);

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

/* One reading in mV at the pin, or -1 when the conversion fails (it then counts as outside). */
static int32_t pilot_mv(void)
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

/* One check: three readings, 3 ms after each; present when two are inside the window. */
static bool pilot_check(void)
{
	int32_t mv[OSC2_DOCK_PILOT_READS];
	uint8_t inside;
	bool present;

	for (int i = 0; i < OSC2_DOCK_PILOT_READS; i++) {
		mv[i] = pilot_mv();
		k_sleep(K_MSEC(OSC2_DOCK_PILOT_READ_MS));
	}
	present = osc2_dock_pilot_present(mv, &inside);
	if (present) {
		osc2_test_note_dock_pilot(mv, inside, present);   /* not every failed check */
	}
	return present;
}

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
			LOG_WRN("Dock: receive error after %d bytes", n);
		}
		outcome = osc2_dock_receive(rx_buf, (size_t)n, tx_buf, NULL);
		if (outcome != OSC2_DOCK_ANSWERED) {
			return;   /* the dock link ended the session */
		}
		k_sem_reset(&tx_done);
		if (uart_tx(uart, tx_buf, sizeof(tx_buf), SYS_FOREVER_US) != 0) {
			/* A send that fails skips this answer; the session goes on. */
			LOG_WRN("Dock: the answer could not be sent");
			continue;
		}
		/* About 6.3 ms on the wire; the receiver is armed again only after it. */
		(void)k_sem_take(&tx_done, K_MSEC(50));
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
	if (!device_is_ready(uart) || !adc_is_ready_dt(&pilot)) {
		LOG_ERR("Dock: the UART or the pilot's ADC is not ready");
		return;
	}
	if (adc_channel_setup_dt(&pilot) != 0 ||
	    uart_callback_set(uart, uart_cb, NULL) != 0) {
		LOG_ERR("Dock: the pilot channel or the UART's async API could not be set up");
		return;
	}
	LOG_INF("Dock: checking the pilot (UNTESTED on hardware)");
	for (;;) {
		if (!osc2_dock_active()) {
			if (!pilot_check()) {
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
