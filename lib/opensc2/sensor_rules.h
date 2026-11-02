/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * sensor_rules.h: a step of the sensor helpers (sensor_helpers.c) that the library's trackpad frames
 * (pad_rules.c) take in one piece. Pure logic, not a public interface.
 */
#ifndef OPENSC2_SENSOR_RULES_H_
#define OPENSC2_SENSOR_RULES_H_

#include <stdbool.h>
#include <stdint.h>
#include <opensc2/sensor_helpers.h>

/* A raw sensor's frame before the pad processing, as the real controller's trackpad driver works
 * it out: in single precision from the centroid (*x, *y, millionths) to the position the frame
 * reports. With `centroid` the edge push, then while `stretch` the stretch of `offset_scale`
 * ({ X offset, X scale, Y offset, Y scale }); with `level_given` the touch from `level` with
 * settings 72 and 73 and the damping near lift-off (`t`), else `touched` as it is. The position is
 * limited to -1..1 and turned back into millionths only at the end, as the driver reports it, so a
 * centroid pushed past an edge is damped as it is. Returns the touch. */
bool osc2_pad_sensor_frame(struct opensc2_pad_touch *t, bool centroid, bool stretch,
			   const float offset_scale[4], bool level_given, int32_t level,
			   int16_t setting72, int16_t setting73, bool touched, int32_t *x, int32_t *y);

#endif /* OPENSC2_SENSOR_RULES_H_ */
