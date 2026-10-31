/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * float_rules.h: float-to-integer conversions as the nRF52's floating-point unit does them
 * (VCVT): toward zero, limited to the target's range, a NaN as 0. A plain C cast leaves an
 * out-of-range value undefined (an x86 PC gives INT32_MIN), so the library's arithmetic that must
 * come out as on the real controller converts with these, on the board and in the host tests
 * alike. Pure logic, not a public interface.
 */
#ifndef OPENSC2_FLOAT_RULES_H_
#define OPENSC2_FLOAT_RULES_H_

#include <stdint.h>

/* To a signed 32-bit integer. */
static inline int32_t osc2_f32_to_s32(float v)
{
	if (v != v) {
		return 0;
	}
	if (v >= 2147483648.0f) {
		return INT32_MAX;
	}
	if (v <= -2147483648.0f) {
		return INT32_MIN;
	}
	return (int32_t)v;
}

/* To an unsigned 32-bit integer: below 0 (and -0.x) as 0. */
static inline uint32_t osc2_f32_to_u32(float v)
{
	if (v != v || v <= 0.0f) {
		return 0;
	}
	if (v >= 4294967296.0f) {
		return UINT32_MAX;
	}
	return (uint32_t)v;
}

#endif /* OPENSC2_FLOAT_RULES_H_ */
