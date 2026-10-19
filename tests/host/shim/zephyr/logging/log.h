/*
 * Copyright (c) 2026 the OpenSC2-Controller authors
 * SPDX-License-Identifier: MIT
 *
 * Host tests only: logging compiled out.
 */
#ifndef OSC2_HOST_SHIM_LOG_H_
#define OSC2_HOST_SHIM_LOG_H_

#define LOG_MODULE_DECLARE(...)
#define LOG_MODULE_REGISTER(...)
#define LOG_DBG(...) ((void)0)
#define LOG_INF(...) ((void)0)
#define LOG_WRN(...) ((void)0)
#define LOG_ERR(...) ((void)0)

#endif /* OSC2_HOST_SHIM_LOG_H_ */
