/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * i2c_recover.c: free every I2C bus once at boot (CONFIG_APP_I2C_RECOVER, on whenever the board
 * has an I2C bus; untested on hardware), after the I2C drivers start and before the sensor and input drivers talk to their
 * parts, as the real controller does: a reset in the middle of a transfer can leave a part
 * holding SDA low, which blocks the bus until SCL is clocked a few times (Zephyr's
 * i2c_recover_bus()).
 */
#include <zephyr/kernel.h>
#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/i2c.h>
#include <zephyr/init.h>
#include <zephyr/logging/log.h>

LOG_MODULE_REGISTER(app_i2c, LOG_LEVEL_INF);

#if defined(CONFIG_APP_I2C_RECOVER)

#define RECOVER(node)                                                                    \
	do {                                                                             \
		const struct device *dev = DEVICE_DT_GET(node);                          \
		int err = device_is_ready(dev) ? i2c_recover_bus(dev) : -ENODEV;         \
                                                                                         \
		if (err && err != -ENOSYS) {                                             \
			LOG_WRN("%s: bus recovery failed (%d)", dev->name, err);         \
		}                                                                        \
	} while (0);

static int recover_buses(void)
{
	DT_FOREACH_STATUS_OKAY(nordic_nrf_twim, RECOVER)
	DT_FOREACH_STATUS_OKAY(nordic_nrf_twi, RECOVER)
	return 0;
}

/* After the I2C drivers (CONFIG_I2C_INIT_PRIORITY), before the sensors and touch modules. */
BUILD_ASSERT(CONFIG_APP_I2C_RECOVER_PRIORITY > CONFIG_I2C_INIT_PRIORITY,
	     "the buses are freed after their drivers start");
SYS_INIT(recover_buses, POST_KERNEL, CONFIG_APP_I2C_RECOVER_PRIORITY);

#endif /* CONFIG_APP_I2C_RECOVER */
