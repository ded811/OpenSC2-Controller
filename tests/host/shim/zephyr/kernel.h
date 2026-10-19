/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: the handful of kernel definitions the library's command channel (commands.c,
 * internal.h) uses, so it builds on a PC. Not Zephyr: no threads, locks that do nothing, a clock
 * the tests set.
 */
#ifndef OSC2_HOST_SHIM_KERNEL_H_
#define OSC2_HOST_SHIM_KERNEL_H_

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define BIT(n)          (1UL << (n))
#define ARRAY_SIZE(a)   (sizeof(a) / sizeof((a)[0]))
#define ARG_UNUSED(x)   (void)(x)
#ifndef MIN
#define MIN(a, b)       ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b)       ((a) > (b) ? (a) : (b))
#endif
#define CLAMP(v, lo, hi) ((v) < (lo) ? (lo) : ((v) > (hi) ? (hi) : (v)))
#define __printf_like(f, a) __attribute__((format(printf, f, a)))

struct k_spinlock {
	int unused;
};
typedef int k_spinlock_key_t;

static inline k_spinlock_key_t k_spin_lock(struct k_spinlock *l)
{
	(void)l;
	return 0;
}

static inline void k_spin_unlock(struct k_spinlock *l, k_spinlock_key_t key)
{
	(void)l;
	(void)key;
}

/* The tests' clock, in milliseconds. */
int64_t k_uptime_get(void);

#endif /* OSC2_HOST_SHIM_KERNEL_H_ */
