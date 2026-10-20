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

/* A mutex that only counts (one thread here): the command channel's and the dock link's. The
 * runner checks after every test that each one taken was let go again (the count is in
 * stubs.c). */
typedef struct {
	int unused;
} k_timeout_t;
#define K_FOREVER ((k_timeout_t){ 0 })

struct k_mutex {
	int depth;
};
#define K_MUTEX_DEFINE(name) struct k_mutex name = { 0 }

extern int osc2_host_mutexes_held;

static inline int k_mutex_lock(struct k_mutex *m, k_timeout_t timeout)
{
	(void)timeout;
	m->depth++;
	osc2_host_mutexes_held++;
	return 0;
}

static inline int k_mutex_unlock(struct k_mutex *m)
{
	m->depth--;
	osc2_host_mutexes_held--;
	return 0;
}

/* The tests' clock, in milliseconds. */
int64_t k_uptime_get(void);

#endif /* OSC2_HOST_SHIM_KERNEL_H_ */
